package com.blackjack.net;

import com.blackjack.MainApp.LobbyRow;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/**
 * NetClient
 *
 * Purpose:
 *   TCP client for the Blackjack server protocol.
 *
 * Responsibilities:
 *   - Maintain a socket connection and a background reader thread.
 *   - Parse server messages and dispatch them to a {@link ProtocolListener}.
 *   - Send client commands (name, lobby join, game actions).
 *   - Perform keep-alive (PING/PONG) and best-effort reconnect.
 *
 * Threading:
 *   - {@link #startReader(ProtocolListener)} spawns a daemon thread that performs blocking reads.
 *   - {@link #sendRaw(String)} is synchronized to serialize socket writes.
 */
public class NetClient {
    private final String host;
    private final int port;
    private final int connectTimeoutMs;

    // Toggle low-level network I/O debug printing (very noisy).
    private static final boolean DEBUG_IO = false;

    // Client-side timeouts are what make the UI react quickly when the server process/machine disappears.
    // Keep HEARTBEAT_INTERVAL_MS <= SERVER_SILENCE_TIMEOUT_MS to avoid false "server not responding"
    // while the connection is simply idle.
    private static final int SOCKET_READ_TIMEOUT_MS = 1000;
    private static final int HEARTBEAT_INTERVAL_MS = 3000;
    private static final int SERVER_SILENCE_TIMEOUT_MS = 10000;
    private static final int PONG_RESPONSE_TIMEOUT_MS = 10000;
    private static final int HANDSHAKE_TIMEOUT_MS = 4000;
    private static final int LOBBY_SNAPSHOT_TIMEOUT_MS = 16000;
    private static final int RECONNECT_WINDOW_MS = 16000;
    private static final int RECONNECT_MAX_ATTEMPTS = 8;
    private static final int RECONNECT_CONNECT_TIMEOUT_MS = 1000;

    private Socket socket;
    private OutputStream os;
    private BufferedReader br;
    private Thread reader;
    private volatile boolean closing = false;
    private volatile String lastName = null;
    private volatile int lastLobby = -1;
    private volatile State state = State.WAIT_OK;
    private volatile boolean expectLobbySnapshot = false;
    private volatile long lastServerMessageMs = System.currentTimeMillis();
    private volatile long lastPingMs = 0L;
    private volatile long lastPingSentMs = 0L;
    private volatile boolean awaitingPong = false;
    private volatile long handshakeSentMs = 0L;
    private volatile boolean handshakeDone = false;
    private volatile String lastConnectFailureHint = null;
    private volatile long lobbySnapshotExpectedMs = 0L;

    private enum State {
        WAIT_OK,
        WAIT_LOBBIES,
        LOBBY_CHOICE,
        LOBBY_WAIT_OR_GAME,
        IN_GAME,
        AFTER_GAME
    }

    private static final class ProtocolException extends Exception {
        ProtocolException(String message) { super(message); }
    }

    /**
     * Convert a connect/reconnect failure into a user-facing hint.
     *
     * This is used to differentiate between "server is down" and "your network is down"
     * (e.g. unplugged cable / Wi‑Fi off), because both look like "no responses" on an
     * already-established TCP connection.
     *
     * @param ex Failure exception (may have nested causes).
     * @return Human-friendly hint string, or null if no specific hint can be derived.
     */
    private static String hintFromConnectFailure(Exception ex) {
        if (ex == null) return null;

        Throwable t = ex;
        while (t != null) {
            if (t instanceof UnknownHostException) {
                return "Cannot resolve server address (check DNS / network).";
            }
            if (t instanceof SocketTimeoutException) {
                return "Connection timed out (check your network).";
            }

            String msg = t.getMessage();
            String m = (msg == null) ? "" : msg.toLowerCase(Locale.ROOT);

            if (t instanceof ConnectException) {
                if (m.contains("refused")) return "Server is offline (connection refused).";
                if (m.contains("network is unreachable") || m.contains("no route")) {
                    return "No network connection (check cable / Wi‑Fi).";
                }
                if (!m.isBlank()) return "Unable to connect to server: " + msg;
                return "Unable to connect to server.";
            }
            if (t instanceof SocketException) {
                if (m.contains("network is unreachable") || m.contains("no route")) {
                    return "No network connection (check cable / Wi‑Fi).";
                }
                if (m.contains("connection reset") || m.contains("broken pipe")) {
                    return "Connection lost.";
                }
            }

            t = t.getCause();
        }

        return null;
    }

    /**
     * Build the final user-facing error message after automatic reconnect fails.
     *
     * @return Human-friendly error message.
     */
    private String autoReconnectFailedMessage() {
        String hint = lastConnectFailureHint;
        String base =
                "Unable to restore connection automatically (" +
                        RECONNECT_MAX_ATTEMPTS + " attempts / " + (RECONNECT_WINDOW_MS / 1000) +
                        " seconds). Please reconnect manually.";
        if (hint == null || hint.isBlank()) return base;
        return hint + " " + base;
    }

    /**
     * Create a NetClient around an already-connected socket.
     *
     * @param s         Connected socket.
     * @param host      Host used for reconnect attempts.
     * @param port      Port used for reconnect attempts.
     * @param timeoutMs Connect timeout for new connections.
     */
    private NetClient(Socket s, String host, int port, int timeoutMs) throws IOException {
        this.host = host;
        this.port = port;
        this.connectTimeoutMs = timeoutMs;
        replaceConnection(s);
    }

    /**
     * Connect to a server and create a {@link NetClient}.
     *
     * @param host      Server host/IP.
     * @param port      Server port.
     * @param timeoutMs Connect timeout in milliseconds.
     * @return Connected NetClient instance.
     */
    public static NetClient connect(String host, int port, int timeoutMs) throws IOException {
        Socket s = new Socket();
        s.connect(new InetSocketAddress(host, port), timeoutMs);
        return new NetClient(s, host, port, timeoutMs);
    }

    /**
     * Start the background reader thread (idempotent).
     *
     * The reader thread:
     *   - reads server lines,
     *   - maintains heartbeat state,
     *   - dispatches events to the provided listener,
     *   - may attempt to reconnect on failures.
     *
     * @param l Listener that receives parsed protocol events.
     */
    public synchronized void startReader(ProtocolListener l) {
        if (reader != null && reader.isAlive()) return;
        reader = new Thread(() -> {
            lastServerMessageMs = System.currentTimeMillis();
            lastPingMs = 0L;
            while (!closing) {
                try {
                    String line = br.readLine();
                    if (line == null){
                        throw new EOFException("server closed");
                        }
                    String t = line.trim();
                    lastServerMessageMs = System.currentTimeMillis();
                    if (DEBUG_IO) System.out.println(t);
                    if (t.isEmpty()) continue;

                    long now = lastServerMessageMs;
                    if (state == State.WAIT_LOBBIES &&
                            lobbySnapshotExpectedMs > 0 &&
                            now - lobbySnapshotExpectedMs > LOBBY_SNAPSHOT_TIMEOUT_MS &&
                            !t.startsWith("C45LOBBIES")) {
                        l.onServerError("Server did not send lobby list.");
                        closeQuietly();
                        return;
                    }

                    if (t.startsWith("C45PONG")) {
                        awaitingPong = false;
                        continue;
                    }
                    if (t.startsWith("C45PING")) {
                        try { sendPong(); } catch (IOException ignored) {}
                        continue;
                    }
                    if (t.startsWith("C45SERVER_DOWN")) {
                        String reason = t.substring("C45SERVER_DOWN".length()).trim();
                        String msg = reason.isEmpty()
                                ? "Server shut down"
                                : "Server shut down: " + reason;
                        l.onServerError(msg);
                        closeQuietly();
                        return;
                    }
                    if (t.startsWith("C45RECONNECT_OK")) {
                        // We are back on the server. It can either resume the game (hand snapshot)
                        // or (if the game already ended) send a normal lobby snapshot.
                        state = State.LOBBY_WAIT_OR_GAME;
                        expectLobbySnapshot = true;
                        handshakeDone = true;
                        continue;
                    }
                    if (t.startsWith("C45OPPDOWN")) {
                        ensureStateIn(t, State.LOBBY_WAIT_OR_GAME, State.IN_GAME, State.AFTER_GAME);
                        String[] p = t.split("\\s+");
                        String who = (p.length >= 2) ? p[1] : "Enemy";
                        int sec = 30;
                        if (p.length >= 3) sec = parsePositiveInt(p[2], "reconnect seconds");
                        l.onOpponentDisconnected(who, sec);
                        continue;
                    }
                    if (t.startsWith("C45OPPBACK")) {
                        ensureStateIn(t, State.LOBBY_WAIT_OR_GAME, State.IN_GAME);
                        String[] p = t.split("\\s+");
                        String who = (p.length >= 2) ? p[1] : "Enemy";
                        l.onOpponentReconnected(who);
                        continue;
                    }

                    if (!t.startsWith("C45")) {
                        throw new ProtocolException("Bad server message (no C45 prefix): " + t);
                    }

                    if (t.startsWith("C45OK")) {
                        if (state == State.WAIT_OK) {
                            ensureState(t, State.WAIT_OK);
                            state = State.WAIT_LOBBIES;
                            expectLobbySnapshot = true;
                            lobbySnapshotExpectedMs = System.currentTimeMillis();
                            l.onOk();
                            handshakeDone = true;
                        } else if (state == State.LOBBY_CHOICE) {
                            ensureState(t, State.LOBBY_CHOICE);
                            state = State.LOBBY_WAIT_OR_GAME;
                            l.onLobbyJoinOk();
                        } else {
                            throw new ProtocolException("Unexpected C45OK in state " + state + ": " + t);
                        }
                        continue;
                    }
                    if(t.startsWith("C45WRONG NAME_TAKEN")){
                        l.onServerError("Name has been taken");
                        closeQuietly();
                        return;
                    }
                    if (t.startsWith("C45WRONG") || t.startsWith("WRONG")) {
                        l.onServerError("WRONG");
                        closeQuietly();
                        return;
                    }

                    // Lobby snapshot
                    if (t.startsWith("C45LOBBIES")) {
                        if (!expectLobbySnapshot && state != State.WAIT_LOBBIES) {
                            throw new ProtocolException("Unexpected lobby snapshot: " + t);
                        }
                        String[] p = t.split("\\s+");
                        if (p.length < 2) throw new ProtocolException("Bad C45LOBBIES header: " + t);
                        int n = parsePositiveInt(p[1], "lobby count");
                        if (n > 100) throw new ProtocolException("Too many lobbies: " + n);
                        List<LobbyRow> rows = new ArrayList<>();
                        for (int i = 0; i < n; i++) {
                            String ln = br.readLine();
                            if (ln == null) throw new EOFException("server closed during snapshot");
                            rows.add(parseLobbyLine(ln.trim()));
                        }
                        l.onLobbySnapshot(rows);
                        state = State.LOBBY_CHOICE;
                        expectLobbySnapshot = false;
                        lobbySnapshotExpectedMs = 0L;
                        continue;
                    }

                    // gameplay
                    if (t.startsWith("C45DEAL")) {
                        String[] p = t.split("\\s+");
                        if (p.length < 3) throw new ProtocolException("Bad C45DEAL: " + t);
                        ensureStateIn(t, State.LOBBY_WAIT_OR_GAME, State.IN_GAME);
                        l.onDeal(p[1], p[2]);
                        state = State.IN_GAME;
                        continue;
                    }
                    if (t.startsWith("C45TURN")) {
                        String[] p = t.split("\\s+");
                        if (p.length < 3) throw new ProtocolException("Bad C45TURN: " + t);
                        ensureStateIn(t, State.IN_GAME);
                        String who = (p.length >= 2) ? p[1] : "?";
                        int sec = parsePositiveInt(p[2], "turn seconds");
                        if (sec > 300) throw new ProtocolException("Bad turn seconds: " + sec);
                        l.onTurn(who, sec); continue;
                    }
                    if (t.startsWith("C45CARD")) {
                        String[] p = t.split("\\s+");
                        ensureStateIn(t, State.IN_GAME);
                        if (p.length >= 2) l.onCard(p[1]); continue;
                    }
                    if (t.startsWith("C45BUST")) {
                        String[] p = t.split("\\s+");
                        if (p.length < 3) throw new ProtocolException("Bad C45BUST: " + t);
                        ensureStateIn(t, State.IN_GAME);
                        if (p.length >= 3) l.onBust(p[1], Integer.parseInt(p[2])); continue;
                    }
                    if (t.startsWith("C45TIMEOUT")) {
                        ensureStateIn(t, State.IN_GAME);
                        continue;
                    }
                    if (t.startsWith("C45RESULT")) {
                        ensureStateIn(t, State.LOBBY_WAIT_OR_GAME, State.IN_GAME);
                        String data = t.substring("C45RESULT".length()).trim();
                        String[] parts = data.split(" ");
                        if (parts.length < 6) throw new ProtocolException("Bad C45RESULT: " + t);
                        String p1     = parts[0];
                        String score1 = parts[1];
                        String p2     = parts[2];
                        String score2 = parts[3];
                        String winner = parts[5];

                        String header = winner.equalsIgnoreCase("PUSH")
                                ? "Draw. Nobody won."
                                : "Winner: " + winner;
                        String result =
                                header + "\n" +
                                        p1 + ": " + score1 + "\n" +
                                        p2 + ": " + score2;

                        l.onResult(result);
                        state = State.AFTER_GAME;
                        continue;
                    }
                    if (t.startsWith("C45WAITING")) {
                        ensureStateIn(t, State.LOBBY_WAIT_OR_GAME);
                        try {
                            sendYes();
                        } catch (IOException ioe) {
                            l.onServerError("send C45YES failed: " + ioe.getMessage());
                        }
                        continue;
                    }

                    if (t.startsWith("C45END")) {
                        continue;
                    }

                    throw new ProtocolException("Unknown server message: " + t);
                } catch (SocketTimeoutException ste) {
                    if (closing) return;
                    long now = System.currentTimeMillis();
                    if (state == State.WAIT_LOBBIES &&
                            lobbySnapshotExpectedMs > 0 &&
                            now - lobbySnapshotExpectedMs > LOBBY_SNAPSHOT_TIMEOUT_MS) {
                        l.onServerError("Server did not send lobby list.");
                        closeQuietly();
                        return;
                    }
                    if (!handshakeDone && handshakeSentMs > 0 &&
                            now - handshakeSentMs > HANDSHAKE_TIMEOUT_MS) {
                        if (tryReconnect(RECONNECT_WINDOW_MS, RECONNECT_MAX_ATTEMPTS)) continue;
                        l.onServerError(autoReconnectFailedMessage());
                        closeQuietly();
                        return;
                    }

                    if (handshakeDone) {
                        if (now - lastPingMs >= HEARTBEAT_INTERVAL_MS && !awaitingPong) {
                            try { sendPing(); } catch (IOException ignored) {}
                            lastPingMs = now;
                        }

                        // Primary signal: we sent PING but didn't receive PONG in time.
                        if (awaitingPong && (now - lastPingSentMs > PONG_RESPONSE_TIMEOUT_MS)) {
                            if (tryReconnect(RECONNECT_WINDOW_MS, RECONNECT_MAX_ATTEMPTS)) continue;
                            l.onServerError(autoReconnectFailedMessage());
                            closeQuietly();
                            return;
                        }

                        // Fallback: no server messages at all for too long.
                        if (now - lastServerMessageMs > SERVER_SILENCE_TIMEOUT_MS) {
                            if (tryReconnect(RECONNECT_WINDOW_MS, RECONNECT_MAX_ATTEMPTS)) continue;
                            l.onServerError(autoReconnectFailedMessage());
                            closeQuietly();
                            return;
                        }
                    } else if (handshakeSentMs == 0) {
                        // Connected, but the user didn't send a name yet. Keep the connection alive and
                        // detect a dead server quickly by using PING/PONG even before the handshake.
                        if (now - lastPingMs >= HEARTBEAT_INTERVAL_MS && !awaitingPong) {
                            try { sendPing(); } catch (IOException ignored) {}
                            lastPingMs = now;
                        }
                        if (now - lastServerMessageMs > SERVER_SILENCE_TIMEOUT_MS) {
                            // No name yet -> cannot use protocol reconnect, but we can still provide a hint
                            // about local network state by probing a fresh TCP connect attempt.
                            String hint = probeConnectHint();
                            l.onServerError(hint != null ? hint : "Server is not responding");
                            closeQuietly();
                            return;
                        }
                    }
                    continue;
                } catch (ProtocolException | NumberFormatException ex) {
                    if (closing) return;
                    l.onServerError(ex.getMessage());
                    closeQuietly();
                    return;
                } catch (Exception ex) {
                    if (closing) return;
                    if (tryReconnect(RECONNECT_WINDOW_MS, RECONNECT_MAX_ATTEMPTS)) {

                        continue;
                    }
                    if (lastName != null && !lastName.isBlank() && lastLobby > 0) {
                        l.onServerError(autoReconnectFailedMessage());
                    } else {
                        String hint = lastConnectFailureHint;
                        l.onServerError(hint != null ? hint : ex.getMessage());
                    }
                    return;
                }
            }
        }, "net-reader");
        reader.setDaemon(true);
        reader.start();
    }

    /* --- Client -> server commands --- */
    /**
     * Send a raw text line to the server.
     *
     * The line must include the trailing {@code \n} according to the server protocol.
     *
     * @param s Line to send.
     */
    public synchronized void sendRaw(String s) throws IOException {
        if (DEBUG_IO) System.out.println(s);
        os.write(s.getBytes(StandardCharsets.UTF_8)); os.flush(); }

    /**
     * Send the initial handshake containing the player name.
     *
     * @param name Player name (must not contain whitespace).
     */
    public void sendName(String name) throws IOException {
        lastName = name;
        handshakeDone = false;
        handshakeSentMs = System.currentTimeMillis();
        state = State.WAIT_OK;
        expectLobbySnapshot = true;
        lobbySnapshotExpectedMs = 0L;
        sendRaw("C45" + name + "\n");
    }

    /**
     * Send a lobby join request using the legacy command format: {@code C45<name><lobby>\n}.
     *
     * @param name  Player name.
     * @param lobby 1-based lobby number.
     */
    public void sendJoin(String name, int lobby) throws IOException {
        lastName = name;
        lastLobby = lobby;
        state = State.LOBBY_CHOICE;
        sendRaw("C45" + name + lobby + "\n");
    }

    /** Send {@code C45HIT}. */
    public void sendHit() throws IOException { sendRaw("C45HIT\n"); }
    /** Send {@code C45STAND}. */
    public void sendStand() throws IOException { sendRaw("C45STAND\n"); }
    /** Send {@code C45YES}. */
    public void sendYes() throws IOException { sendRaw("C45YES\n"); }
    /** Send {@code C45PONG}. */
    public void sendPong() throws IOException { sendRaw("C45PONG\n"); }
    /** Send {@code C45PING}. */
    public void sendPing() throws IOException {
        long now = System.currentTimeMillis();
        sendRaw("C45PING\n");
        awaitingPong = true;
        lastPingSentMs = now;
    }

    /**
     * Request going back to lobby list.
     *
     * @param name Player name.
     */
    public void sendBackToLobby(String name) throws IOException {
        expectLobbySnapshot = true;
        sendRaw("C45" + (name == null ? "" : name) + "back\n");
    }

    /**
     * Explicitly request a reconnect on the server.
     *
     * @param name  Player name.
     * @param lobby 1-based lobby number.
     */
    public void sendReconnect(String name, int lobby) throws IOException {
        if (name == null) name = "";
        lastName = name;
        lastLobby = lobby;
        handshakeDone = false;
        handshakeSentMs = System.currentTimeMillis();
        state = State.WAIT_OK;
        expectLobbySnapshot = true;
        lobbySnapshotExpectedMs = 0L;
        sendRaw("C45RECONNECT " + name + " " + lobby + "\n");
    }

    /**
     * Close the socket and stop the reader thread (best effort).
     */
    public void closeQuietly() {
        // Close socket first to unblock reader thread; closing BufferedReader can block on its internal lock.
        closing = true;
        try { socket.close(); } catch (Exception ignored) {}
        try { if (reader != null) reader.interrupt(); } catch (Exception ignored) {}
    }

    /**
     * Replace the current connection with a new socket and reset per-connection state.
     *
     * @param s Connected socket.
     */
    private synchronized void replaceConnection(Socket s) throws IOException {
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        s.setSoTimeout(SOCKET_READ_TIMEOUT_MS);
        s.setKeepAlive(true);
        s.setTcpNoDelay(true);
        this.socket = s;
        this.os = s.getOutputStream();
        this.br = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
        lastServerMessageMs = System.currentTimeMillis();
        lastPingMs = 0L;
        lastPingSentMs = 0L;
        awaitingPong = false;
        lobbySnapshotExpectedMs = 0L;
    }

    /**
     * Try to reconnect within the given time window.
     *
     * @param windowMs Max time spent reconnecting.
     * @return true if reconnection succeeded; false otherwise.
     */
    private boolean tryReconnect(long windowMs, int maxAttempts) {
        String n = lastName;
        int lobby = lastLobby;
        if (n == null || n.isBlank()) return false;

        // After reconnect we can either land back in the running game OR be redirected to lobby list.
        state = State.WAIT_OK;
        expectLobbySnapshot = true;
        handshakeDone = false;
        lobbySnapshotExpectedMs = 0L;

        long deadline = System.currentTimeMillis() + Math.max(0L, windowMs);
        long perAttemptMs = Math.max(1L, windowMs / Math.max(1, maxAttempts));
        Exception last = null;
        int attempts = 0;
        while (!closing && attempts < maxAttempts && System.currentTimeMillis() < deadline) {
            long attemptStart = System.currentTimeMillis();
            try {
                attempts++;
                if (DEBUG_IO) System.out.println("Reconnect attempt: " + attempts);
                Socket s = new Socket();
                int to = Math.min(connectTimeoutMs, RECONNECT_CONNECT_TIMEOUT_MS);
                long remaining = deadline - System.currentTimeMillis();
                if (remaining < to) to = (int)Math.max(1L, remaining);
                s.connect(new InetSocketAddress(host, port), to);
                replaceConnection(s);
                handshakeSentMs = System.currentTimeMillis();
                if (lobby > 0) {
                    sendRaw("C45RECONNECT " + n + " " + lobby + "\n");
                } else {
                    // Reconnect even if the user hasn't selected a lobby yet.
                    // The server treats lobby=0 as "resume to lobby list if not in a game".
                    sendRaw("C45RECONNECT " + n + " 0\n");
                }
                lastConnectFailureHint = null;
                return true;
            } catch (Exception ex) {
                last = ex;
                long elapsed = System.currentTimeMillis() - attemptStart;
                long sleepMs = perAttemptMs - elapsed;
                if (sleepMs > 0) {
                    long remaining = deadline - System.currentTimeMillis();
                    if (remaining <= 0) break;
                    try { Thread.sleep(Math.min(sleepMs, remaining)); }
                    catch (InterruptedException ie) { break; }
                }
            }
        }

        lastConnectFailureHint = hintFromConnectFailure(last);
        return false;
    }

    /**
     * Probe a fresh TCP connect attempt to provide a better error message.
     *
     * Used when we don't have enough state to perform a protocol reconnect (no nickname yet).
     *
     * @return Hint string, or null if no hint is available.
     */
    private String probeConnectHint() {
        try {
            Socket s = new Socket();
            int to = Math.min(connectTimeoutMs, RECONNECT_CONNECT_TIMEOUT_MS);
            s.connect(new InetSocketAddress(host, port), to);
            try { s.close(); } catch (Exception ignored) {}
            return null;
        } catch (Exception ex) {
            return hintFromConnectFailure(ex);
        }
    }

    /**
     * Parse a single lobby line from the server snapshot.
     *
     * Expected format:
     *   {@code C45LOBBY <id> players=<n>/<cap> status=<0|1>}
     *
     * @param line Raw line (without trailing newline).
     * @return Parsed {@link LobbyRow}.
     */
    private LobbyRow parseLobbyLine(String line) throws ProtocolException {
        String[] parts = line.split("\\s+");
        if (parts.length != 4 || !parts[0].equalsIgnoreCase("C45LOBBY")) {
            throw new ProtocolException("Bad lobby line: " + line);
        }
        int id = parsePositiveInt(parts[1], "lobby id");

        String[] pp = parts[2].split("=");
        if (pp.length != 2 || !pp[0].equalsIgnoreCase("players")) {
            throw new ProtocolException("Bad lobby players: " + line);
        }
        String[] ps = pp[1].split("/");
        if (ps.length != 2) throw new ProtocolException("Bad lobby players: " + line);
        int players = parseNonNegativeInt(ps[0], "players");
        int cap = parsePositiveInt(ps[1], "capacity");
        if (players > cap) throw new ProtocolException("Bad lobby players: " + line);

        String[] ss = parts[3].split("=");
        if (ss.length != 2 || !ss[0].equalsIgnoreCase("status")) {
            throw new ProtocolException("Bad lobby status: " + line);
        }
        String status = ss[1];
        return new LobbyRow(id, players, cap, status);
    }

    /**
     * Ensure the current client state matches the expected value.
     *
     * @param msg      Original message (used for error context).
     * @param expected Expected state.
     */
    private void ensureState(String msg, State expected) throws ProtocolException {
        if (state != expected) throw new ProtocolException("Bad client state for message: " + msg);
    }

    /**
     * Ensure the current client state is one of the allowed values.
     *
     * @param msg     Original message (used for error context).
     * @param allowed Allowed states.
     */
    private void ensureStateIn(String msg, State... allowed) throws ProtocolException {
        for (State s : allowed) if (state == s) return;
        throw new ProtocolException("Bad client state for message: " + msg);
    }

    /**
     * Parse a strictly positive integer (v > 0).
     *
     * @param s   Input string.
     * @param ctx Context string used for error message.
     * @return Parsed integer value.
     */
    private static int parsePositiveInt(String s, String ctx) throws ProtocolException {
        try {
            int v = Integer.parseInt(s);
            if (v <= 0) throw new NumberFormatException();
            return v;
        } catch (NumberFormatException ex) {
            throw new ProtocolException("Bad " + ctx + ": " + s);
        }
    }

    /**
     * Parse a non-negative integer (v >= 0).
     *
     * @param s   Input string.
     * @param ctx Context string used for error message.
     * @return Parsed integer value.
     */
    private static int parseNonNegativeInt(String s, String ctx) throws ProtocolException {
        try {
            int v = Integer.parseInt(s);
            if (v < 0) throw new NumberFormatException();
            return v;
        } catch (NumberFormatException ex) {
            throw new ProtocolException("Bad " + ctx + ": " + s);
        }
    }

    /* --- C45 framed messages (optional) --- */

    /**
     * Build a framed C45 line: {@code C45LL<payload>\n}.
     *
     * @param payload Payload string (NULL is treated as empty).
     * @return Full frame line.
     */
    private static String buildC45Frame(String payload) {
        if (payload == null) payload = "";
        int len = payload.length();
        if (len > 99) throw new IllegalArgumentException("C45 payload too long: " + len);
        return String.format("C45%02d%s\n", len, payload);
    }

    /**
     * Try to parse a framed C45 line and return its payload.
     *
     * @param line Raw line (without trailing newline).
     * @return Payload string if this is a valid frame; null if not a C45 frame.
     */
    private static String tryParseC45Frame(String line) throws IOException {
        if (line == null || !line.startsWith("C45")) return null;
        if (line.length() < 5) throw new IOException("Bad C45 frame: too short");
        char a = line.charAt(3), b = line.charAt(4);
        if (!Character.isDigit(a) || !Character.isDigit(b)) throw new IOException("Bad C45 length digits");
        int len = (a - '0') * 10 + (b - '0');
        if (len < 0 || len > 99) throw new IOException("C45 length out of range");
        String payload = line.substring(5);
        if (payload.length() != len) throw new IOException("C45 length mismatch");
        return payload;
    }

    /**
     * Compatibility helper: convert a payload into the legacy token form ("C45" + payload).
     *
     * @param maybePayloadOrNull Parsed payload (or null).
     * @return Legacy token string (or null).
     */
    private static String coerceToLegacyToken(String maybePayloadOrNull) {
        if (maybePayloadOrNull == null) return null;
        return "C45" + maybePayloadOrNull;
    }

}
