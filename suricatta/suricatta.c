/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <util.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <json-c/json.h>
#include "pctl.h"
#include "suricatta/suricatta.h"
#include "suricatta/server.h"
#include "suricatta_private.h"
#include "parselib.h"
#include <network_ipc.h>

static bool enable = true;

void suricatta_print_help(void)
{
	server.help();
}

static server_op_res_t suricatta_enable(ipc_message *msg)
{
	struct json_object *json_root;
	json_object *json_data;

	json_root = server_tokenize_msg(msg->data.instmsg.buf,
					sizeof(msg->data.instmsg.buf));
	if (!json_root) {
		msg->type = NACK;
		ERROR("Wrong JSON message, see documentation");
		return SERVER_EERR;
	}

	json_data = json_get_path_key(
	    json_root, (const char *[]){"enable", NULL});
	if (json_data) {
		enable = json_object_get_boolean(json_data);
		TRACE ("suricatta mode %sabled", enable ? "en" : "dis");
	}

	msg->type = ACK;

	return SERVER_OK;
}

static server_op_res_t suricatta_ipc(int fd, time_t *seconds)
{
	ipc_message msg;
	server_op_res_t result = SERVER_OK;
	int ret;

	ret = read(fd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		return SERVER_EERR;

	switch (msg.data.instmsg.cmd) {
	case CMD_ENABLE:
		result = suricatta_enable(&msg);
		/*
		 * Note: enable works as trigger, too.
		 * After enable is set, suricatta will try to contact
		 * the server to check for pending action
		 * This is done by resetting the number of seconds to
		 * wait for.
		 */
		*seconds = 0;
		break;
	default:
		result = server.ipc(&msg);
		break;
	}

	if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
		TRACE("IPC ERROR: sending back msg");
	}

	/* Send ipc back */
	return result;
}

int suricatta_wait(int seconds)
{
	fd_set readfds;
	struct timeval tv;
	int retval;

	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	FD_ZERO(&readfds);
	FD_SET(sw_sockfd, &readfds);
	DEBUG("Sleeping for %ld seconds.", tv.tv_sec);
	retval = select(sw_sockfd + 1, &readfds, NULL, NULL, &tv);
	if (retval < 0) {
		TRACE("Suricatta awakened because of: %s", strerror(errno));
		return 0;
	}
	if (retval && FD_ISSET(sw_sockfd, &readfds)) {
		TRACE("Suricatta woke up for IPC at %ld seconds", tv.tv_sec);
		if (suricatta_ipc(sw_sockfd, &tv.tv_sec) != SERVER_OK){
			DEBUG("Handling IPC failed!");
		}
		return (int)tv.tv_sec;
	}
	return 0;
}

int start_suricatta(const char *cfgfname, int argc, char *argv[])
{
	int action_id;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigpipe_mask, &saved_mask);

	if (server.start(cfgfname, argc, argv) != SERVER_OK) {
		exit(EXIT_FAILURE);
	}

	TRACE("Server initialized, entering suricatta main loop.");
	while (true) {
		if (enable) {
			switch (server.has_pending_action(&action_id)) {
			case SERVER_UPDATE_AVAILABLE:
				DEBUG("About to process available update.");
				server.install_update();
				break;
			case SERVER_ID_REQUESTED:
				server.send_target_data();
				break;
			case SERVER_EINIT:
				break;
			case SERVER_OK:
			default:
				DEBUG("No pending action to process.");
				break;
			}
		}

		for (int wait_seconds = server.get_polling_interval();
			 wait_seconds > 0;
			 wait_seconds = min(wait_seconds, (int)server.get_polling_interval())) {
			wait_seconds = suricatta_wait(wait_seconds);
		}

		TRACE("Suricatta awakened.");
	}
}
