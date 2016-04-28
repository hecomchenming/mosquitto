/*
Copyright (c) 2009-2014 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

#define _GNU_SOURCE

#include <config.h>

#include <assert.h>
#include <poll.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#ifdef WITH_WEBSOCKETS
#  include <libwebsockets.h>
#endif

#include <mosquitto_broker.h>
#include <memory_mosq.h>
#include <send_mosq.h>
#include <time_mosq.h>
#include <util_mosq.h>
#include <stdlib.h>
#include <sys/epoll.h>

extern bool flag_reload;
#ifdef WITH_PERSISTENCE
extern bool flag_db_backup;
#endif
extern bool flag_tree_print;
extern int run;
#ifdef WITH_SYS_TREE
extern int g_clients_expired;
#endif

static mosq_sock_t * _listen_socks;
static int _listen_count;

static void handle_events(struct mosquitto_db *db, struct epoll_event *epoll_events, int event_count);

#ifdef WITH_WEBSOCKETS
static void temp__expire_websockets_clients(struct mosquitto_db *db)
{
	struct mosquitto *context, *ctxt_tmp;
	static time_t last_check = 0;
	time_t now = mosquitto_time();
	char *id;

	if(now - last_check > 60){
		HASH_ITER(hh_id, db->contexts_by_id, context, ctxt_tmp){
			if(context->wsi && context->sock != INVALID_SOCKET){
				if(context->keepalive && now - context->last_msg_in > (time_t)(context->keepalive)*3/2){
					if(db->config->connection_messages == true){
						if(context->id){
							id = context->id;
						}else{
							id = "<unknown>";
						}
						_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s has exceeded timeout, disconnecting.", id);
					}
					/* Client has exceeded keepalive*1.5 */
					do_disconnect(db, context);
				}
			}
		}
		last_check = mosquitto_time();
	}
}
#endif

static void _add_event(int epollfd, int fd, int state)
{
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
	_mosquitto_log_printf(NULL, MOSQ_LOG_DEBUG, "add sock %d", fd);
}

static void _modify_event(int epollfd, int fd, int state)
{
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
	_mosquitto_log_printf(NULL, MOSQ_LOG_DEBUG, "mod sock %d", fd);
}

static void _delete_event(int epollfd, int fd, int state) {
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
	_mosquitto_log_printf(NULL, MOSQ_LOG_DEBUG, "del sock %d", fd);
}

/* 从大到小 */
static int _compare_socks(const void *s1, const void *s2) {
    mosq_sock_t *sock1 = (mosq_sock_t *)s1;
    mosq_sock_t *sock2 = (mosq_sock_t *)s2;
    return (*sock2) - (*sock1);
}

/* 二分查找 */
static int _bi_find_sock(mosq_sock_t *socks, int sock_count, mosq_sock_t target) {
    int low = 0;
    int high = sock_count - 1;
    while (low <= high) {
        int mid = (low + high) >> 1;
        if (socks[mid] == target) {
            return 1;
        } else if (socks[mid] > target) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
	_mosquitto_log_printf(NULL, MOSQ_LOG_DEBUG, "sock %d not found", target);
    return 0;
}


int mosquitto_main_loop(struct mosquitto_db *db, mosq_sock_t *listensock, int listensock_count, int listener_max)
{
#ifdef WITH_SYS_TREE
	time_t start_time = mosquitto_time();
#endif
#ifdef WITH_PERSISTENCE
	time_t last_backup = mosquitto_time();
#endif
	time_t now = 0;
	time_t now_time;
	int time_count;
	struct mosquitto *context, *ctxt_tmp;
	sigset_t sigblock, origsig;
	int i;
#ifdef WITH_BRIDGE
	mosq_sock_t bridge_sock;
	int rc;
#endif
	int context_count;
	time_t expiration_check_time = 0;
	time_t last_timeout_check = 0;
	char *id;
    
    struct epoll_event event;
    struct epoll_event *events;
    int event_count;
    
    _listen_socks = listensock;
    _listen_count = listensock_count;

	sigemptyset(&sigblock);
	sigaddset(&sigblock, SIGINT);
    
    qsort(listensock, listensock_count, sizeof(mosq_sock_t), _compare_socks);
	_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "sock0: %d, sockn-1: %d", listensock[0], listensock[listensock_count - 1]);

	if(db->config->persistent_client_expiration > 0){
		expiration_check_time = time(NULL) + 3600;
	}
    
    db->efd = epoll_create1 (0);
    if (db->efd == -1)
    {
        perror ("epoll_create");
        abort ();
    }
    
    for (i = 0; i < listensock_count; ++i)
    {
        _add_event(db->efd, listensock[i], EPOLLIN);
    }
    
    /* Buffer where events are returned */
    events = calloc (MAX_CONNECT, sizeof event);

	while(run){
		mosquitto__free_disused_contexts(db);
#ifdef WITH_SYS_TREE
		if(db->config->sys_interval > 0){
			mqtt3_db_sys_update(db, db->config->sys_interval, start_time);
		}
#endif

		context_count = HASH_CNT(hh_sock, db->contexts_by_sock);
#ifdef WITH_BRIDGE
		context_count += db->bridge_count;
#endif

		now_time = time(NULL);

		time_count = 0;
		HASH_ITER(hh_sock, db->contexts_by_sock, context, ctxt_tmp){
			if(time_count > 0){
				time_count--;
			}else{
				time_count = 1000;
				now = mosquitto_time();
			}

			if(context->sock != INVALID_SOCKET){
#ifdef WITH_BRIDGE
				if(context->bridge){
					_mosquitto_check_keepalive(db, context);
					if(context->bridge->round_robin == false
							&& context->bridge->cur_address != 0
							&& now > context->bridge->primary_retry){

						if(_mosquitto_try_connect(context, context->bridge->addresses[0].address, context->bridge->addresses[0].port, &bridge_sock, NULL, false) <= 0){
							COMPAT_CLOSE(bridge_sock);
							_mosquitto_socket_close(db, context);
							context->bridge->cur_address = context->bridge->address_count-1;
						}
					}
				}
#endif

				/* Local bridges never time out in this fashion. */
				if(!(context->keepalive)
						|| context->bridge
						|| now - context->last_msg_in < (time_t)(context->keepalive)*3/2){

					if(mqtt3_db_message_write(db, context) == MOSQ_ERR_SUCCESS){
						if(context->current_out_packet || context->state == mosq_cs_connect_pending){
							_modify_event(db->efd, context->sock, EPOLLIN|EPOLLOUT);
						}
					}else{
						do_disconnect(db, context);
					}
				}else{
					if(db->config->connection_messages == true){
						if(context->id){
							id = context->id;
						}else{
							id = "<unknown>";
						}
						_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s has exceeded timeout, disconnecting.", id);
					}
					/* Client has exceeded keepalive*1.5 */
					do_disconnect(db, context);
				}
			}
		}

#ifdef WITH_BRIDGE
		time_count = 0;
		for(i=0; i<db->bridge_count; i++){
			if(!db->bridges[i]) continue;

			context = db->bridges[i];

			if(context->sock == INVALID_SOCKET){
				if(time_count > 0){
					time_count--;
				}else{
					time_count = 1000;
					now = mosquitto_time();
				}
				/* Want to try to restart the bridge connection */
				if(!context->bridge->restart_t){
					context->bridge->restart_t = now+context->bridge->restart_timeout;
					context->bridge->cur_address++;
					if(context->bridge->cur_address == context->bridge->address_count){
						context->bridge->cur_address = 0;
					}
					if(context->bridge->round_robin == false && context->bridge->cur_address != 0){
						context->bridge->primary_retry = now + 5;
					}
				}else{
					if(context->bridge->start_type == bst_lazy && context->bridge->lazy_reconnect){
						rc = mqtt3_bridge_connect(db, context);
						if(rc){
							context->bridge->cur_address++;
							if(context->bridge->cur_address == context->bridge->address_count){
								context->bridge->cur_address = 0;
							}
                        } else {
                            _add_event(db->efd, context->sock, EPOLLIN);
                        }
					}
					if(context->bridge->start_type == bst_automatic && now > context->bridge->restart_t){
						context->bridge->restart_t = 0;
						rc = mqtt3_bridge_connect(db, context);
						if(rc == MOSQ_ERR_SUCCESS){
							if(context->current_out_packet){
								_add_event(db->efd, context->sock, EPOLLIN|EPOLLOUT);
                            } else {
                                _add_event(db->efd, context->sock, EPOLLIN);
                            }
						}else{
							/* Retry later. */
							context->bridge->restart_t = now+context->bridge->restart_timeout;

							context->bridge->cur_address++;
							if(context->bridge->cur_address == context->bridge->address_count){
								context->bridge->cur_address = 0;
							}
						}
					}
				}
			}
		}
#endif
		now_time = time(NULL);
		if(db->config->persistent_client_expiration > 0 && now_time > expiration_check_time){
			HASH_ITER(hh_id, db->contexts_by_id, context, ctxt_tmp){
				if(context->sock == INVALID_SOCKET && context->clean_session == 0){
					/* This is a persistent client, check to see if the
					 * last time it connected was longer than
					 * persistent_client_expiration seconds ago. If so,
					 * expire it and clean up.
					 */
					if(now_time > context->disconnect_t+db->config->persistent_client_expiration){
						if(context->id){
							id = context->id;
						}else{
							id = "<unknown>";
						}
						_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Expiring persistent client %s due to timeout.", id);
#ifdef WITH_SYS_TREE
						g_clients_expired++;
#endif
						context->clean_session = true;
						context->state = mosq_cs_expiring;
						do_disconnect(db, context);
					}
				}
			}
			expiration_check_time = time(NULL) + 3600;
		}

		if(last_timeout_check < mosquitto_time()){
			/* Only check at most once per second. */
			mqtt3_db_message_timeout_check(db, db->config->retry_interval);
			last_timeout_check = mosquitto_time();
		}

		sigprocmask(SIG_SETMASK, &sigblock, &origsig);
		event_count = epoll_wait(db->efd, events, MAX_EVENT, -1);
		sigprocmask(SIG_SETMASK, &origsig, NULL);

		if(event_count == -1){
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error in poll: %s.", strerror(errno));
		}else{
		_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "to handle_events: %d", event_count);
            handle_events(db, events, event_count);
		}
#ifdef WITH_PERSISTENCE
		if(db->config->persistence && db->config->autosave_interval){
			if(db->config->autosave_on_changes){
				if(db->persistence_changes >= db->config->autosave_interval){
					mqtt3_db_backup(db, false);
					db->persistence_changes = 0;
				}
			}else{
				if(last_backup + db->config->autosave_interval < mosquitto_time()){
					mqtt3_db_backup(db, false);
					last_backup = mosquitto_time();
				}
			}
		}
#endif

#ifdef WITH_PERSISTENCE
		if(flag_db_backup){
			mqtt3_db_backup(db, false);
			flag_db_backup = false;
		}
#endif
		if(flag_reload){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Reloading config.");
			mqtt3_config_read(db->config, true);
			mosquitto_security_cleanup(db, true);
			mosquitto_security_init(db, true);
			mosquitto_security_apply(db);
			mqtt3_log_close(db->config);
			mqtt3_log_init(db->config);
			flag_reload = false;
		}
		if(flag_tree_print){
			mqtt3_sub_tree_print(&db->subs, 0);
			flag_tree_print = false;
		}
#ifdef WITH_WEBSOCKETS
		for(i=0; i<db->config->listener_count; i++){
			/* Extremely hacky, should be using the lws provided external poll
			 * interface, but their interface has changed recently and ours
			 * will soon, so for now websockets clients are second class
			 * citizens. */
			if(db->config->listeners[i].ws_context){
				libwebsocket_service(db->config->listeners[i].ws_context, 0);
			}
		}
		if(db->config->have_websockets_listener){
			temp__expire_websockets_clients(db);
		}
#endif
	}

    if(events) {
        _mosquitto_free(events);
    }
	return MOSQ_ERR_SUCCESS;
}

void do_disconnect(struct mosquitto_db *db, struct mosquitto *context)
{
	char *id;

	if(context->state == mosq_cs_disconnected){
		return;
	}
    
    _delete_event(db->efd, context->sock, EPOLLIN);
    
#ifdef WITH_WEBSOCKETS
	if(context->wsi){
		if(context->state != mosq_cs_disconnecting){
			context->state = mosq_cs_disconnect_ws;
		}
		if(context->wsi){
			libwebsocket_callback_on_writable(context->ws_context, context->wsi);
		}
		context->sock = INVALID_SOCKET;
	}else
#endif
	{
		if(db->config->connection_messages == true){
			if(context->id){
				id = context->id;
			}else{
				id = "<unknown>";
			}
			if(context->state != mosq_cs_disconnecting){
				_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Socket error on client %s, disconnecting.", id);
			}else{
				_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s disconnected.", id);
			}
		}
		mqtt3_context_disconnect(db, context);
#ifdef WITH_BRIDGE
		if(context->clean_session && !context->bridge){
#else
		if(context->clean_session){
#endif
			mosquitto__add_context_to_disused(db, context);
			if(context->id){
				HASH_DELETE(hh_id, db->contexts_by_id, context);
				_mosquitto_free(context->id);
				context->id = NULL;
			}
		}
		context->state = mosq_cs_disconnected;
	}
}

static void handle_events(struct mosquitto_db *db, struct epoll_event *epoll_events, int events_count) {
    struct mosquitto *context;
    int err;
    socklen_t len;
    int i;
    int fd;
    int new_sock;
    
    for (i = 0; i < events_count; ++i) {
        fd = epoll_events[i].data.fd;
        
        HASH_FIND(hh_sock, db->contexts_by_sock, &(epoll_events[i].data.fd), sizeof(int), context);
        if (context == NULL) {
            if (epoll_events[i].events & (EPOLLIN|EPOLLPRI) && _bi_find_sock(_listen_socks, _listen_count, fd)) {
                while(new_sock = mqtt3_socket_accept(db, fd), new_sock != -1){
                }
            }
            continue;
        }
        
        if (epoll_events[i].events & (EPOLLERR | EPOLLHUP)) {
            do_disconnect(db, context);
            continue;
        }
        
        assert(context->sock == epoll_events[i].data.fd);
        
#ifdef WITH_TLS
        if(epoll_events[i].events & EPOLLOUT ||
           context->want_write ||
           (context->ssl && context->state == mosq_cs_new)){
#else
        if(epoll_events[i].events & EPOLLOUT){
#endif
            if(context->state == mosq_cs_connect_pending){
                len = sizeof(int);
                if(!getsockopt(context->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len)){
                    if(err == 0){
                        context->state = mosq_cs_new;
                    }
                }else{
                    do_disconnect(db, context);
                    continue;
                }
            }
            if(_mosquitto_packet_write(context)){
                do_disconnect(db, context);
                continue;
            }
        }
#ifdef WITH_TLS
        if(epoll_events[i].events & EPOLLIN ||
               (context->ssl && context->state == mosq_cs_new)){
#else
        if (epoll_events[i].events & EPOLLIN) {
#endif
            do{
                if(_mosquitto_packet_read(db, context)){
                    do_disconnect(db, context);
                    continue;
                }
            }while(SSL_DATA_PENDING(context));
        }
    }
}
