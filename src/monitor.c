/*
  Copyright (c) 2016 James Hunt

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
 */

#include "pgrouter.h"
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>

#define SUBSYS "monitor"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

static void handle_client(CONTEXT *c, int connfd)
{
	int rc, i;

	rdlock(&c->lock, "context", 0);

	pgr_sendf(connfd, "backends %d/%d\n", c->ok_backends, c->num_backends);
	pgr_sendf(connfd, "workers %d\n", c->workers);
	pgr_sendf(connfd, "clients %d\n", c->fe_conns);

	for (i = 0; i < c->num_backends; i++) {
		rdlock(&c->backends[i].lock, "backend", i);

		switch (c->backends[i].status) {
		case BACKEND_IS_OK:
			pgr_sendf(connfd, "%s:%d %s %s %llu/%llu\n",
					c->backends[i].hostname, c->backends[i].port,
					pgr_backend_role(c->backends[i].role),
					pgr_backend_status(c->backends[i].status),
					c->backends[i].health.lag,
					c->backends[i].health.threshold);
			break;

		default:
			pgr_sendf(connfd, "%s:%d %s %s\n",
					c->backends[i].hostname, c->backends[i].port,
					pgr_backend_role(c->backends[i].role),
					pgr_backend_status(c->backends[i].status));
			break;
		}

		unlock(&c->backends[i].lock, "backend", i);
	}

	unlock(&c->lock, "context", 0);
}

static void* do_monitor(void *_c)
{
	CONTEXT *c = (CONTEXT*)_c;
	int rc, connfd, i, nfds;
	int watch[2] = { c->monitor4, c->monitor6 };
	fd_set rfds;

	for (;;) {
		FD_ZERO(&rfds);
		nfds = 0;

		for (i = 0; i < sizeof(watch)/sizeof(watch[0]); i++) {
			if (watch[i] >= 0) {
				FD_SET(watch[i], &rfds);
				nfds = max(watch[i], nfds);
			}
		}

		rc = select(nfds+1, &rfds, NULL, NULL, NULL);
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			pgr_logf(stderr, LOG_ERR, "[monitor] select received system error: %s (errno %d)",
					strerror(errno), errno);
			pgr_abort(ABORT_SYSCALL);
		}

		for (i = 0; i < sizeof(watch)/sizeof(watch[0]); i++) {
			if (watch[i] >= 0 && FD_ISSET(watch[i], &rfds)) {
				char remote_addr[INET6_ADDRSTRLEN+1];
				struct sockaddr_storage peer;
				int peer_len = sizeof(peer);

				connfd = accept(watch[i], (struct sockaddr*)&peer, &peer_len);
				switch (peer.ss_family) {
				case AF_INET:
					memset(remote_addr, 0, sizeof(remote_addr));
					pgr_logf(stderr, LOG_INFO, "[monitor] inbound connection from %s:%d",
							inet_ntop(AF_INET, &((struct sockaddr_in*)&peer)->sin_addr,
								remote_addr, sizeof(remote_addr)),
								((struct sockaddr_in*)&peer)->sin_port);
					break;

				case AF_INET6:
					memset(remote_addr, 0, sizeof(remote_addr));
					pgr_logf(stderr, LOG_INFO, "[monitor] inbound connection from %s:%d",
							inet_ntop(AF_INET6, &((struct sockaddr_in6*)&peer)->sin6_addr,
								remote_addr, sizeof(remote_addr)),
								((struct sockaddr_in6*)&peer)->sin6_port);
					break;
				}
				handle_client(c, connfd);
				close(connfd);
			}
		}
	}

	close(c->monitor4);
	close(c->monitor6);
	return NULL;
}

int pgr_monitor(CONTEXT *c, pthread_t *tid)
{
	int rc = pthread_create(tid, NULL, do_monitor, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return;
	}

	pgr_logf(stderr, LOG_INFO, "[monitor] spinning up [tid=%i]", *tid);
}
