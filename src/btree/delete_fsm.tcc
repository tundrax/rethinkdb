
#ifndef __BTREE_DELETE_FSM_TCC__
#define __BTREE_DELETE_FSM_TCC__

#include "utils.hpp"
#include "cpu_context.hpp"

template <class config_t>
void btree_delete_fsm<config_t>::init_delete(btree_key *_key) {
    keycpy(key, _key);
    state = start_transaction;
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_start_transaction(event_t *event) {
    assert(state == start_transaction);

    /* Either start a new transaction or retrieve the one we started. */
    assert(transaction == NULL);
    if (event == NULL) {
        transaction = cache->begin_transaction(rwi_write, this);
    } else {
        assert(event->buf); // We shouldn't get a callback unless this is valid
        transaction = (typename config_t::transaction_t *)event->buf;
    }

    /* Determine our forward progress based on our new state. */
    if (transaction) {
        state = acquire_superblock;
        return btree_fsm_t::transition_ok;
    } else {
        return btree_fsm_t::transition_incomplete; // Flush lock is held.
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_superblock(event_t *event)
{
    assert(state == acquire_superblock);

    if(event == NULL) {
        // First entry into the FSM; try to grab the superblock.
        block_id_t superblock_id = btree_fsm_t::get_cache()->get_superblock_id();
        sb_buf = transaction->acquire(superblock_id, rwi_write, this);
    } else {
        // We already tried to grab the superblock, and we're getting
        // a cache notification about it.
        assert(event->buf);
        sb_buf = (buf_t *)event->buf;
    }

    if(sb_buf) {
        // Got the superblock buffer (either right away or through
        // cache notification). Grab the root id, and move on to
        // acquiring the root.
        node_id = btree_fsm_t::get_root_id(sb_buf->ptr());
        if(cache_t::is_block_id_null(node_id)) {
            op_result = btree_not_found;
            state = delete_complete;
            sb_buf->release();
            sb_buf = NULL;
        } else {
            state = acquire_root;
        }
        return btree_fsm_t::transition_ok;
    } else {
        // Can't get the superblock buffer right away. Let's wait for
        // the cache notification.
        return btree_fsm_t::transition_incomplete;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_root(event_t *event) {
    assert(state == acquire_root);
    
    // Make sure root exists
    if(cache_t::is_block_id_null(node_id)) {
        op_result = btree_not_found;
        state = delete_complete;
        return btree_fsm_t::transition_ok;
    }

    if(event == NULL) {
        // Acquire the actual root node
        buf = transaction->acquire(node_id, rwi_write, this);
    } else {
        // We already tried to grab the root, and we're getting a
        // cache notification about it.
        assert(event->buf);
        buf = (buf_t*)event->buf;
    }
    
    if(buf == NULL) {
        // Can't grab the root right away. Wait for a cache event.
        return btree_fsm_t::transition_incomplete;
    } else {
        // Got the root, move on to grabbing the node
        state = acquire_node;
        return btree_fsm_t::transition_ok;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_insert_root_on_collapse(event_t *event)
{
    if(set_root_id(node_id, event)) {
        state = acquire_node;
        sb_buf->release();
        sb_buf = NULL;
        return btree_fsm_t::transition_ok;
    } else {
        return btree_fsm_t::transition_incomplete;
    }
}


template <class config_t>
void btree_delete_fsm<config_t>::split_internal_node(buf_t *buf, buf_t **rbuf,
                                         block_id_t *rnode_id, btree_key *median) {
    buf_t *res = transaction->allocate(rnode_id);
    assert(node_handler::is_internal((node_t *)buf->ptr()));
    internal_node_t *node = (internal_node_t *)buf->ptr();
    internal_node_t *rnode = (internal_node_t *)res->ptr();
    internal_node_handler::split(node, rnode, median);
    buf->set_dirty();
    res->set_dirty();
    *rbuf = res;
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_insert_root_on_split(event_t *event) {
    if(set_root_id(last_node_id, event)) {
        state = acquire_node;
        sb_buf->release();
        sb_buf = NULL;
        return btree_fsm_t::transition_ok;
    } else {
        return btree_fsm_t::transition_incomplete;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_node(event_t *event) {
    assert(state == acquire_node);
    // Either we already have the node (then event should be NULL), or
    // we don't have the node (in which case we asked for it before,
    // and it should be getting to us via an event)
    //assert((buf && !event) || (!buf && event));

    if (!event) {
        buf = transaction->acquire(node_id, rwi_write, this);
    } else {
        assert(event->buf);
        buf = (buf_t*) event->buf;
    }

    if(buf)
        return btree_fsm_t::transition_ok;
    else
        return btree_fsm_t::transition_incomplete;
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_acquire_sibling(event_t *event) {
    assert(state == acquire_sibling);

    if (!event) {
        assert(last_buf);
        node_t *last_node = (node_t *)last_buf->ptr();

        internal_node_handler::sibling(((internal_node_t*)last_node), key, &sib_node_id);
        sib_buf = transaction->acquire(sib_node_id, rwi_write, this);
    } else {
        assert(event->buf);
        sib_buf = (buf_t*) event->buf;
    }

    if(sib_buf) {
        state = acquire_node;
        return btree_fsm_t::transition_ok;
    } else {
        return btree_fsm_t::transition_incomplete;
    }
}

template <class config_t>
typename btree_delete_fsm<config_t>::transition_result_t btree_delete_fsm<config_t>::do_transition(event_t *event) {
    transition_result_t res = btree_fsm_t::transition_ok;

    // Make sure we've got either an empty or a cache event
    check("btree_fsm::do_transition - invalid event",
          !(!event || event->event_type == et_cache));

    // Update the cache with the event
    if(event && event->event_type == et_cache) {
        check("btree_delete _fsm::do_transition - invalid event", event->op != eo_read);
        check("Could not complete AIO operation",
              event->result == 0 ||
              event->result == -1);
    }

    // First, begin a transaction.
    if(res == btree_fsm_t::transition_ok && state == start_transaction) {
        res = do_start_transaction(event);
        event = NULL;
    }

    // Next, acquire the superblock (to get root node ID)
    if(res == btree_fsm_t::transition_ok && state == acquire_superblock) {
        assert(transaction); // We must have started our transaction by now.
        res = do_acquire_superblock(event);
        event = NULL;
    }
        
    // Then, acquire the root block
    if(res == btree_fsm_t::transition_ok && state == acquire_root) {
        res = do_acquire_root(event);
        event = NULL;
    }

    // If the previously acquired node was underfull, we acquire a sibling to level or merge
    if(res == btree_fsm_t::transition_ok && state == acquire_sibling) {
        res = do_acquire_sibling(event);
        event = NULL;
    }
    
    // If we need to change the root due to merging it's last two children, do that
    if(res == btree_fsm_t::transition_ok && state == insert_root_on_collapse) {
        res = do_insert_root_on_collapse(event);
        event = NULL;
    }

    //printf("-----\n");
    // Acquire nodes
    while(res == btree_fsm_t::transition_ok && state == acquire_node) {
        if(!buf) {
            state = acquire_node;
            res = do_acquire_node(event);
            event = NULL;
            if(res != btree_fsm_t::transition_ok || state != acquire_node) {
                break;
            }
        }



        node_handler::validate((node_t*)buf->ptr());
        if (node_handler::is_internal((node_t*)buf->ptr())) {
            // Internal node
            internal_node_t* node = (internal_node_t*)buf->ptr();
            //printf("internal start\n");
            if (internal_node_handler::is_underfull((internal_node_t *)node) && last_buf) { // the root node is never considered underfull
                if(!sib_buf) { // Acquire a sibling to merge or level with
                    //printf("internal acquire sibling\n");
                    state = acquire_sibling;
                    res = do_acquire_sibling(NULL);
                    event = NULL;
                    continue;
                } else {
                    // Sibling acquired, now decide whether to merge or level
                    internal_node_t *sib_node = (internal_node_t*)sib_buf->ptr();
                    node_handler::validate(sib_node);
                    internal_node_t *parent_node = (internal_node_t*)last_buf->ptr();
                    if ( internal_node_handler::is_mergable(node, sib_node, parent_node)) {
                        // Merge
                        //printf("internal merge\n");
                        btree_key *key_to_remove = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE); //TODO get alloca outta here
                        if (internal_node_handler::nodecmp(node, sib_node) < 0) { // Nodes must be passed to merge in ascending order
                            internal_node_handler::merge(node, sib_node, key_to_remove, parent_node);
                            buf->release(); //TODO delete when api is ready
                            buf = sib_buf;
                            node_id = sib_node_id;
                        } else {
                            internal_node_handler::merge(sib_node, node, key_to_remove, parent_node);
                            sib_buf->release(); //TODO delete when api is ready
                        }
                        sib_buf = NULL;

                        if (!internal_node_handler::is_singleton(parent_node)) {
                            internal_node_handler::remove(parent_node, key_to_remove);
                        } else {
                            //printf("internal collapse root\n");
                            // parent has only 1 key (which means it is also the root), replace it with the node
                            // when we get here node_id should be the id of the new root
                            state = insert_root_on_collapse;
                            res = do_insert_root_on_collapse(NULL);
                            event = NULL;
                            continue;
                        }
                    } else {
                        // Level
                        //printf("internal level\n");
                        btree_key *key_to_replace = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE);
                        btree_key *replacement_key = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE);
                        bool leveled = internal_node_handler::level(node,  sib_node, key_to_replace, replacement_key, parent_node);

                        if (leveled) {
                            //set everyone dirty
                            sib_buf->set_dirty();

                            internal_node_handler::update_key(parent_node, key_to_replace, replacement_key);
                            last_buf->set_dirty();

                            buf->set_dirty();
                        }
                        sib_buf->release();
                        sib_buf = NULL;
                    }
                }
            } else if (internal_node_handler::is_full(node)) {
                //internal node is full - must be proactively split (because merges/levels can change stored key sizes)
                //printf("internal split\n");
                btree_key *median = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE);
                buf_t *rbuf;
                internal_node_t *last_node;
                block_id_t rnode_id;
                bool new_root = false;
                split_internal_node(buf, &rbuf, &rnode_id, median);
                // Create a new root if we're splitting a root
                if(last_buf == NULL) {
                    new_root = true;
                    last_buf = transaction->allocate(&last_node_id);
                    last_node = (internal_node_t *)last_buf->ptr();
                    internal_node_handler::init(last_node);
                    last_buf->set_dirty();
                } else {
                    last_node = (internal_node_t *)last_buf->ptr();
                }
                bool success = internal_node_handler::insert(last_node, median, node_id, rnode_id);
                check("could not insert internal btree node", !success);
                last_buf->set_dirty();

                // Figure out where the key goes
                if(sized_strcmp(key->contents, key->size, median->contents, median->size) <= 0) {
                    // Left node and node are the same thing
                    rbuf->release();
                    rbuf = NULL;
                } else {
                    buf->release();
                    buf = rbuf;
                    rbuf = NULL;
                    node = (internal_node_t *)buf->ptr();
                    node_id = rnode_id;
                }

                if(new_root) {
                    state = insert_root_on_split;
                    res = do_insert_root_on_split(NULL);
                    if(res != btree_fsm_t::transition_ok || state != acquire_node) {
                        break;
                    }
                }
            }

            // Release the superblock if we're past the root node
            if(sb_buf && last_buf) {
                sb_buf->release();
                sb_buf = NULL;
            }

            // Acquire next node
            if(!cache_t::is_block_id_null(last_node_id) && last_buf) {
                last_buf->release();
            }
            last_buf = buf;
            last_node_id = node_id;

            node_id = internal_node_handler::lookup(node, key);
            buf = NULL;

            res = do_acquire_node(event);
            event = NULL;
        } else {
            // Leaf node
            leaf_node_t* node = (leaf_node_t*)buf->ptr();
            //printf("leaf start\n");
            // If we haven't already, do some deleting 
            if (op_result == btree_incomplete) {
                if(leaf_node_handler::remove(node, key)) {
                    //key found, and value deleted
                    buf->set_dirty();
                    op_result = btree_found;
                } else {
                    //key not found, nothing deleted
                    op_result = btree_not_found;
                }
            }

            if (leaf_node_handler::is_underfull(node) && last_buf) { // the root node is never underfull

                if (leaf_node_handler::is_empty(node)) {
                    //printf("leaf empty\n");
                    internal_node_t *parent_node = (internal_node_t*)last_buf->ptr();
                    if (!internal_node_handler::is_singleton(parent_node)) {
                        internal_node_handler::remove(parent_node, key);
                        //TODO: delete buf when api is ready
                    } else {
                        // the root contains only two nodes, one of which is empty.  delete the empty one, and make the other one the root
                        //TODO: delete buf when api is ready
                        if(!sib_buf) {
                            // Acquire the only sibling to make into the root
                            state = acquire_sibling;
                            res = do_acquire_sibling(NULL);
                            event = NULL;
                            continue;
                        } else {
                            node_id = sib_node_id;
                            state = insert_root_on_collapse;
                            res = do_insert_root_on_collapse(NULL);
                            event = NULL;
                            sib_buf->release();
                            sib_buf = NULL;
                        }
                    }
                } else {
                    if(!sib_buf) {
                        //printf("leaf acquire sibling\n");
                        // Acquire a sibling to merge or level with
                        state = acquire_sibling;
                        res = do_acquire_sibling(NULL);
                        event = NULL;
                        continue;
                    } else {
                        // Sibling acquired
                        leaf_node_t *sib_node = (leaf_node_t*)sib_buf->ptr();
                        node_handler::validate(sib_node);
                        internal_node_t *parent_node = (internal_node_t*)last_buf->ptr();
                        // Now decide whether to merge or level
                        if(leaf_node_handler::is_mergable(node, sib_node)) {
                            //printf("leaf merge\n");
                            // Merge
                            btree_key *key_to_remove = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE); //TODO get alloca outta here
                            if (leaf_node_handler::nodecmp(node, sib_node) < 0) { // Nodes must be passed to merge in ascending order
                                leaf_node_handler::merge(node, sib_node, key_to_remove);
                                //TODO: delete buf when api is ready
                                node_id = sib_node_id;
                            } else {
                                leaf_node_handler::merge(sib_node, node, key_to_remove);
                                //TODO: delete sib_buf when api is ready
                            }
                            sib_buf->release();
                            sib_buf = NULL;

                            if (!internal_node_handler::is_singleton(parent_node)) {
                                //normal merge
                                internal_node_handler::remove(parent_node, key_to_remove);
                            } else {
                                //parent has only 1 key (which means it is also the root), replace it with the node
                                // when we get here node_id should be the id of the new root
                                //printf("leaf collapse root\n");
                                state = insert_root_on_collapse;
                                res = do_insert_root_on_collapse(NULL);
                                event = NULL;
                            }
                        } else {
                            //printf("leaf level\n");
                            // Level
                            btree_key *key_to_replace = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE);
                            btree_key *replacement_key = (btree_key *)alloca(sizeof(btree_key) + MAX_KEY_SIZE);
                            bool leveled = leaf_node_handler::level(node,  sib_node, key_to_replace, replacement_key);

                            if (leveled) {
                                //set everyone dirty
                                sib_buf->set_dirty();

                                internal_node_handler::update_key(parent_node, key_to_replace, replacement_key);
                                last_buf->set_dirty();

                                buf->set_dirty();
                            }
                            sib_buf->release();
                            sib_buf = NULL;
                        }
                    }
                }
            }
            // Release the superblock, if we haven't already
            if(sb_buf) {
                sb_buf->release();
                sb_buf = NULL;
            }


            if (last_buf)
                last_buf->release();

            buf->release();
            state = delete_complete;
            res = btree_fsm_t::transition_ok;
        }
    }

    // Finally, end our transaction.  This should always succeed immediately.
    if (res == btree_fsm_t::transition_ok && state == delete_complete) {
        bool committed __attribute__((unused)) = transaction->commit(this);
        state = committing;
        if (committed) {
            transaction = NULL;
            res = btree_fsm_t::transition_complete;
        }
        event = NULL;
    }

    // Finalize the transaction commit
    if(res == btree_fsm_t::transition_ok && state == committing) {
        if (event != NULL) {
            assert(event->event_type == et_commit);
            assert(event->buf == transaction);
            transaction = NULL;
            res = btree_fsm_t::transition_complete;
        }
    }

    assert(res != btree_fsm_t::transition_complete || is_finished());

    return res;
}

template <class config_t>
int btree_delete_fsm<config_t>::set_root_id(block_id_t root_id, event_t *event) {
    sb_buf->set_dirty();
    memcpy(sb_buf->ptr(), (void*)&root_id, sizeof(root_id));
    return 1;
}

template <class config_t> const char* btree_delete_fsm<config_t>::name = "btree_delete_fsm";

#endif // __BTREE_DELETE_FSM_TCC__

