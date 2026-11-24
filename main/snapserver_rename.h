#pragma once

// Sets this client's name on the snapserver. A name saved on the server
// (Snapweb, Music Assistant) wins over the Hello's HostName, so after a new
// snapcast name is saved the board sends JSON-RPC Client.SetName to the
// server's control port (CONFIG_SNAPSERVER_CONTROL_PORT) once. A failure
// stays pending in NVS and is retried at the next boot.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Rename this client on the snapserver at server_ip (IPv4 or IPv6 text).
 * Copies the strings and runs on its own short-lived task: never blocks.
 * A no-op while a rename is already running.
 */
void snapserver_rename_start(const char *server_ip, const char *client_id,
                             const char *name);

/** Mark a rename as due this boot (the saved flag is set). */
void snapserver_rename_set_pending(void);

/**
 * "" when no rename is due this boot, else "pending", "renaming", "done" or
 * "failed". Safe to call from any task.
 */
const char *snapserver_rename_state(void);

#ifdef __cplusplus
}
#endif
