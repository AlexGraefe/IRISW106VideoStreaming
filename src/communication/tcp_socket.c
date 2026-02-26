#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "wifi_utilities.h"
#include "wifi_pswd.h"
#include "tcp_socket.h"

#include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(tcp_socket, LOG_LEVEL_DBG);

#define SERVER_IP   "192.168.5.29"
#define SERVER_PORT 8080
#define BUFFER_SIZE 1024

static const char *messages[] = {
	"Hello, Server!",
	"How are you?",
	"Socket demo working.",
	"Goodbye!",
	NULL
};

typedef enum {
	COMM_WIFI_CONNECTING,
	COMM_WAITING_FOR_IP,
	COMM_ESTABLISHING_SERVER,
	COMM_SENDING_MESSAGES,
	COMM_FAILURE,
	COMM_CLEANUP,
	COMM_DONE,
} communication_state_t;

typedef struct {
	struct sockaddr_in server_addr;
	char buffer[BUFFER_SIZE];
	int sock_fd;
	bool wifi_connected;
	bool socket_open;
	int exit_code;
	communication_state_t failure_from_state;
} communication_context_t;

static const char *state_to_string(communication_state_t state)
{
	switch (state) {
	case COMM_WIFI_CONNECTING:
		return "COMM_WIFI_CONNECTING";
	case COMM_WAITING_FOR_IP:
		return "COMM_WAITING_FOR_IP";
	case COMM_ESTABLISHING_SERVER:
		return "COMM_CONNECTING_TO_SERVER";
	case COMM_SENDING_MESSAGES:
		return "COMM_SENDING_MESSAGES";
	case COMM_FAILURE:
		return "COMM_FAILURE";
	case COMM_CLEANUP:
		return "COMM_CLEANUP";
	case COMM_DONE:
		return "COMM_DONE";
	default:
		return "COMM_UNKNOWN";
	}
}

static communication_state_t state_wifi_connecting(communication_context_t *ctx)
{
	if (my_wifi_init() != 0) {
		LOG_ERR("Failed to initialize WiFi module");
		ctx->failure_from_state = COMM_WIFI_CONNECTING;
		return COMM_FAILURE;
	}

	LOG_INF("Connecting to WiFi...");

	if (wifi_connect(BITCRAZE_SSID, BITCRAZE_PASSWORD)) {
		LOG_ERR("Failed to connect to WiFi");
		ctx->failure_from_state = COMM_WIFI_CONNECTING;
		return COMM_FAILURE;
	}

	ctx->wifi_connected = true;
	return COMM_WAITING_FOR_IP;
}

static communication_state_t state_waiting_for_ip(communication_context_t *ctx)
{
	if (wifi_wait_for_ip_addr() != 0) {
		LOG_ERR("Failed while waiting for IPv4 address");
		ctx->failure_from_state = COMM_WAITING_FOR_IP;
		return COMM_FAILURE;
	}

	return COMM_ESTABLISHING_SERVER;
}

static communication_state_t state_connecting_to_server(communication_context_t *ctx)
{
	int ret;

	ctx->sock_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (ctx->sock_fd < 0) {
		LOG_ERR("Could not create socket (errno=%d)", errno);
		ctx->failure_from_state = COMM_ESTABLISHING_SERVER;
		return COMM_FAILURE;
	}
	ctx->socket_open = true;

	memset(&ctx->server_addr, 0, sizeof(ctx->server_addr));
	ctx->server_addr.sin_family = AF_INET;
	ctx->server_addr.sin_port = htons(SERVER_PORT);

	ret = zsock_inet_pton(AF_INET, SERVER_IP, &ctx->server_addr.sin_addr);
	if (ret != 1) {
		LOG_ERR("Invalid SERVER_IP (%s)", SERVER_IP);
		ctx->failure_from_state = COMM_ESTABLISHING_SERVER;
		return COMM_FAILURE;
	}

	ret = zsock_connect(ctx->sock_fd, (struct sockaddr *)&ctx->server_addr, sizeof(ctx->server_addr));
	if (ret < 0) {
		LOG_ERR("Could not connect to server (errno=%d)", errno);
		ctx->failure_from_state = COMM_ESTABLISHING_SERVER;
		return COMM_FAILURE;
	}

	LOG_INF("[Client] Connected to %s:%d", SERVER_IP, SERVER_PORT);
	return COMM_SENDING_MESSAGES;
}

static communication_state_t state_sending_messages(communication_context_t *ctx)
{
	int ret;

	for (int i = 0; messages[i] != NULL; i++) {
		ret = zsock_send(ctx->sock_fd, messages[i], strlen(messages[i]), 0);
		if (ret < 0) {
			LOG_ERR("send failed (errno=%d)", errno);
			ctx->failure_from_state = COMM_SENDING_MESSAGES;
			return COMM_FAILURE;
		}

		LOG_DBG("[Client] Sent: %s", messages[i]);

		ret = zsock_recv(ctx->sock_fd, ctx->buffer, sizeof(ctx->buffer) - 1, 0);
		if (ret <= 0) {
			if (ret == 0) {
				LOG_WRN("[Client] Server closed the connection");
			} else {
				LOG_ERR("recv failed (errno=%d)", errno);
			}
			ctx->failure_from_state = COMM_SENDING_MESSAGES;
			return COMM_FAILURE;
		}

		ctx->buffer[ret] = '\0';
		LOG_DBG("[Client] Received: %s", ctx->buffer);
	}

	return COMM_CLEANUP;
}

static communication_state_t state_failure(communication_context_t *ctx)
{
	LOG_ERR("[Failure] Called from: %s", state_to_string(ctx->failure_from_state));
	LOG_ERR("[Failure] Context: sock_fd=%d socket_open=%d wifi_connected=%d exit_code=%d",
			ctx->sock_fd,
			ctx->socket_open,
			ctx->wifi_connected,
			ctx->exit_code);

	ctx->exit_code = -1;
	return COMM_CLEANUP;
}

static communication_state_t state_cleanup(communication_context_t *ctx)
{
	if (ctx->socket_open) {
		zsock_close(ctx->sock_fd);
		ctx->socket_open = false;
		LOG_INF("[Client] Closed");
	}

	if (ctx->wifi_connected) {
		wifi_disconnect();
		ctx->wifi_connected = false;
	}

	return COMM_DONE;
}

int run_tcp_socket_demo(void)
{
	communication_context_t ctx = {
		.sock_fd = -1,
		.wifi_connected = false,
		.socket_open = false,
		.exit_code = 0,
		.failure_from_state = COMM_DONE,
	};
	communication_state_t state = COMM_WIFI_CONNECTING;

	LOG_INF("TCP ECHO CLIENT DEMO");

	while (state != COMM_DONE) {
		switch (state) {
		case COMM_WIFI_CONNECTING:
			state = state_wifi_connecting(&ctx);
			break;
		case COMM_WAITING_FOR_IP:
			state = state_waiting_for_ip(&ctx);
			break;
		case COMM_ESTABLISHING_SERVER:
			state = state_connecting_to_server(&ctx);
			break;
		case COMM_SENDING_MESSAGES:
			state = state_sending_messages(&ctx);
			break;
		case COMM_FAILURE:
			state = state_failure(&ctx);
			break;
		case COMM_CLEANUP:
			state = state_cleanup(&ctx);
			break;
		case COMM_DONE:
			break;
		default:
			ctx.failure_from_state = state;
			state = COMM_FAILURE;
			break;
		}
	}

	return ctx.exit_code;
}
