"""The MQTT client setup shared by ingest and the router."""

import logging
import ssl

import paho.mqtt.client as mqtt

log = logging.getLogger(__name__)


def connect(m, client_id: str, topics: list[str], on_message) -> mqtt.Client:
    """Start a client that keeps a persistent session and subscribes to
    `topics` on every connect. paho's thread connects, and keeps reconnecting,
    in the background, even if the broker isn't up yet."""

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("broker refused the connection: %s", reason_code)
            return
        log.info(
            "connected to %s:%d as %s (session present: %s)",
            m.host,
            m.port,
            client_id,
            flags.session_present,
        )
        client.subscribe([(topic, m.qos) for topic in topics])

    def on_disconnect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.warning("disconnected from broker (%s), reconnecting", reason_code)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, clean_session=False)
    if m.username:
        client.username_pw_set(m.username, m.password)
    if m.tls:
        client.tls_set(ca_certs=m.ca_file, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect_async(m.host, m.port, keepalive=m.keepalive_s)
    client.loop_start()
    log.info("subscribing to %s on %s:%d", ", ".join(topics), m.host, m.port)
    return client


def wait_then_disconnect(client: mqtt.Client, stop, parent_gone, tick=None) -> None:
    """Run until the supervisor says stop (or is gone), then disconnect.
    tick(final), if given, is called about once a second, and once more with
    final=True just before disconnecting."""
    while not stop.wait(1.0):
        if parent_gone():
            log.error("supervisor is gone, stopping")
            break
        if tick:
            tick(False)
    if tick:
        tick(True)
    client.disconnect()
    client.loop_stop()
