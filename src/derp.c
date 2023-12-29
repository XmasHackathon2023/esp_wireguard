#include "derp.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <lwip/netif.h>
#include <lwip/ip.h>
#include <lwip/tcp.h>
#include <lwip/mem.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <sys/socket.h>
#include <esp_log.h>
#include <esp_err.h>
#include "crypto.h"
#include "sodium.h"

#define WIREGUARDIF_TIMER_MSECS 400

#define TAG "derp"

void derp_tick(struct wireguard_device *dev) {
	err_t err = ESP_FAIL;

	bool is_any_peer_active = false;
	for (int i = 0; i < WIREGUARD_MAX_PEERS; i++) {
		is_any_peer_active |= dev->peers[i].active;
	}

	if (is_any_peer_active && dev->derp.tcp == NULL) {
		ESP_LOGE(TAG, "No DERP connection, but active peers exists -> initializing DERP connection");
		err = derp_initiate_new_connection(dev);
		ESP_LOGE(TAG, "New DERP connection initiation status, %d", err);
	} else if (!is_any_peer_active && dev->derp.tcp) {
		ESP_LOGE(TAG, "No active peer exists - Shutting down DERP connection");
		err = derp_shutdown_connection(dev);
		ESP_LOGE(TAG, "Shutdown of DERP connection status, %d", err);
	}

	// TODO: if tcp pointer is live, but connection is
	//       dead then free dev->derp.tcp

	// TODO: count how many ticks are spent in each state
	//       (except for DISCONNECTED and DERP_READY) and if
	//       it is above some threshold - reset the connection
	//       by calling derp_shutdown_connection()
}

err_t tcp_connected_callback(void *arg, struct tcp_pcb *tcp, err_t err) {
	struct wireguard_device *dev = (struct wireguard_device *)arg;
	LWIP_ASSERT("tcp_connected_callback: invalid state", dev->derp.tcp == tcp);

	ESP_LOGE(TAG, "Connected callback with status: %d", err);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to connect, resetting: %d", err);
		derp_shutdown_connection(dev);
		return ESP_OK;
	}

	err = derp_send_http_upgrade_request(dev);
	if (err != ESP_OK) {
		derp_shutdown_connection(dev);
		return ESP_OK;
	}

	return ESP_OK;
}

err_t tcp_sent_callback(void *arg, struct tcp_pcb *tcp, u16_t len) {
	struct wireguard_device *dev = (struct wireguard_device *)arg;
	LWIP_ASSERT("tcp_sent_callback: invalid state", dev->derp.tcp == tcp);

	ESP_LOGE(TAG, "TCP has sent %d amount of bytes", len);

	return ESP_OK;
}

err_t tcp_recv_callback(void *arg, struct tcp_pcb *tcp, struct pbuf *buf, err_t err) {
	struct wireguard_device *dev = (struct wireguard_device *)arg;
	LWIP_ASSERT("tcp_sent_callback: invalid state", dev->derp.tcp == tcp);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "TCP has indicated failure at receive callback %d", err);
		derp_shutdown_connection(dev);
		return ESP_FAIL;
	}

	if (buf == NULL) {
		ESP_LOGE(TAG, "Remote end has closed the connection");
		derp_shutdown_connection(dev);
		return ESP_OK;
	}

	if (buf->next != NULL) {
		ESP_LOGE(TAG, "Fragmented payload, such payload is not yet supported");
		derp_shutdown_connection(dev);
		return ESP_OK;
	}

	// This packet is only valid in CONN_STATE_HTTP_KEY_EXHCANGE or CONN_STATE_DERP_READY
	struct derp_pkt *pkt = (struct derp_pkt*)buf->payload;

	switch (dev->derp.conn_state) {
		case CONN_STATE_TCP_DISCONNECTED:
		case CONN_STATE_TCP_CONNECTING:
			ESP_LOGE(TAG, "Received packet during unexpected state: %d", dev->derp.conn_state);
			break;

		case CONN_STATE_HTTP_GET_REQ:
			err = derp_key_exchange(dev, buf);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "Error while processing key exchange packets, resetting DERP conn");
				derp_shutdown_connection(dev);
				return ESP_OK;
			}
			break;

		case CONN_STATE_HTTP_KEY_EXHCANGE:
			if (pkt->type == 3) {
				ESP_LOGE(TAG, "Received packet of type %d in KeyExchange state, DERP connection established succesfully", pkt->type);
				// This message is not super useful for us, so we will just ignore it :P
				dev->derp.conn_state = CONN_STATE_DERP_READY;
			}
			break;

		case CONN_STATE_DERP_READY:
			ESP_LOGE(TAG, "Received packet of type %d in READY state", pkt->type);

			break;
	}

	// Acknowledge all of the received bytes
	tcp_recved(dev->derp.tcp, buf->tot_len);

	// Free the pbuf, as everything went fine
	pbuf_free(buf);

	return ESP_OK;
}

err_t derp_initiate_new_connection(struct wireguard_device *dev) {
	err_t err = ESP_FAIL;

	ESP_LOGE(TAG, "Precoutionary cleanup of DERP connections");
	err = derp_shutdown_connection(dev);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cleanup FAILED");
		return ESP_FAIL;
	}

	LWIP_ASSERT("derp_initiate_new_connection: invalid state", dev->derp.tcp == NULL);

	ESP_LOGE(TAG, "Allocating new socket");
	dev->derp.tcp = tcp_new();
	if (dev->derp.tcp == NULL) {
		ESP_LOGE(TAG, "Failed to allocate socket");
		return ESP_FAIL;
	}

	ESP_LOGE(TAG, "Configuring sent ack callback");
	tcp_sent(dev->derp.tcp, tcp_sent_callback);

	ESP_LOGE(TAG, "Configuring recv callback");
	tcp_recv(dev->derp.tcp, tcp_recv_callback);

	ESP_LOGE(TAG, "Binding device to socket");
	tcp_arg(dev->derp.tcp, dev);

	u16_t sndbuf = tcp_sndbuf(dev->derp.tcp);
	ESP_LOGE(TAG, "TCP SNDBUF Size: %d", sndbuf);

	ESP_LOGE(TAG, "Convert IP address");
	struct ip_addr addr = {0};
	err = ipaddr_aton("157.230.123.169", &addr);
	if (err != 1) {
		ESP_LOGE(TAG, "Failed to convert IP address %d", err);
		return ESP_FAIL;
	}

	ESP_LOGE(TAG, "Attempting to connect to DERP");
	err = tcp_connect(dev->derp.tcp, &addr, 8765, tcp_connected_callback);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "tcp_connect() failed %d", err);
		return ESP_FAIL;
	}

	dev->derp.conn_state = CONN_STATE_TCP_CONNECTING;

	return ESP_OK;
}

err_t derp_shutdown_connection(struct wireguard_device *dev) {
	if (dev->derp.tcp != NULL) {
		tcp_abort(dev->derp.tcp);
		dev->derp.tcp = NULL;
	}

	dev->derp.conn_state = CONN_STATE_TCP_DISCONNECTED;

	return ESP_OK;
}

err_t derp_send_http_upgrade_request(struct wireguard_device *dev) {
	err_t err = ESP_FAIL;

	const char * http_req =
		"GET /derp HTTP/1.1\r\n"
		"Host: 157.230.123.169\r\n"
		"Connection: Upgrade\r\n"
		"Upgrade: WebSocket\r\n"
		"User-Agent: esp32/v1.0.0 esp\r\n\r\n";

	ESP_LOGE(TAG, "Sending HTTP upgrade request");
	err = tcp_write(dev->derp.tcp, http_req, strlen(http_req), 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to send HTTP upgrade request %d", err);
		return ESP_FAIL;
	}

	dev->derp.conn_state = CONN_STATE_HTTP_GET_REQ;

	return ESP_OK;
}

err_t derp_key_exchange(struct wireguard_device *dev, struct pbuf *buf) {
	err_t err = ESP_FAIL;

	// pbuf contains HTTP response and first WebSocket packet
	// Ensure server is ok with upgrading protocol
	char *expected_http_status = "101 Switching Protocols";
	if (pbuf_memfind(buf, expected_http_status, strlen(expected_http_status), 0) == 0xFFFF) {
		ESP_LOGE(TAG, "Server has not responded with success response: %s", buf->payload);
		return ESP_FAIL;
	}

	// Find the end of HTTP response
	char *http_response_end = "\r\n\r\n";
	u16_t idx = pbuf_memfind(buf, http_response_end, strlen(http_response_end), 0);
	if (idx == 0xFFFF) {
		ESP_LOGE(TAG, "Failed to find the end of HTTP response");
		return ESP_FAIL;
	}
	idx += strlen(http_response_end);

	// Verify the length of the response
	const int required_derp_pkt_length = 45;
	const int required_packet_length = idx + required_derp_pkt_length;
	if (buf->tot_len < required_packet_length) {
		ESP_LOGE(TAG, "Received packet is too short: %d < %d", buf->tot_len, required_packet_length);
		return ESP_FAIL;
	}

	struct derp_pkt *server_key_pkt = (struct derp_pkt*)(&((uint8_t*)buf->payload)[idx]);
	ESP_LOGE(TAG, "Received server key server_key_pkt:");
	ESP_LOGE(TAG, "    type      : %d", server_key_pkt->type);
	ESP_LOGE(TAG, "    length    : %d", BE32_TO_LE32(server_key_pkt->length_be));
	ESP_LOGE(TAG, "    magic     : %s", server_key_pkt->server_key.magic);
	ESP_LOGE(TAG, "    public_key: %s", server_key_pkt->server_key.server_public_key);

	//TODO: Validate that the server key is in
	//      fact matching the one provided via config

	// Prepare client_key packet
	struct derp_pkt client_key_pkt;
	memset(&client_key_pkt, 0, sizeof(client_key_pkt));

	const char* plaintext = "{\"version\": 2, \"meshKey\": \"\"}";
	client_key_pkt.type = 2; // Client-Key packet
	client_key_pkt.length_be = BE32_TO_LE32(101);
	memcpy(client_key_pkt.client_key.client_public_key, dev->public_key, sizeof(client_key_pkt.client_key.client_public_key));
	randombytes_buf(client_key_pkt.client_key.nonce, sizeof(client_key_pkt.client_key.nonce));

	ESP_LOGE(TAG, "Attempting to encrypt the plaintext for client-key");
	err = crypto_box_easy(client_key_pkt.client_key.ciphertext,
			plaintext, strlen(plaintext),
			client_key_pkt.client_key.nonce,
			server_key_pkt->server_key.server_public_key,
			dev->private_key);
	if (err != 0) {
		ESP_LOGE(TAG, "Failed to encrypt plaintext for client-key packet: %d", err);
		return ESP_FAIL;
	}

	err = tcp_write(dev->derp.tcp, &client_key_pkt, 106, TCP_WRITE_FLAG_COPY);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to send HTTP upgrade request %d", err);
		return ESP_FAIL;
	}

	dev->derp.conn_state = CONN_STATE_HTTP_KEY_EXHCANGE;

	return ESP_OK;
}

err_t derp_data_message(struct wireguard_device *dev, struct derp_pkt *packet) {
	err_t err = ESP_FAIL;


	return ESP_OK;
}


