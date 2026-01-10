package com.blackjack.net;

import com.blackjack.MainApp.LobbyRow;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class NetClient {
    private final String host;
    private final int port;
    private final int connectTimeoutMs;

    private Socket socket;
    private OutputStream os;
    private BufferedReader br;
    private Thread reader;
    private volatile boolean closing = false;
    private volatile String lastName = null;
    private volatile int lastLobby = -1;

    private NetClient(Socket s, String host, int port, int timeoutMs) throws IOException {
        this.host = host;
        this.port = port;
        this.connectTimeoutMs = timeoutMs;
        replaceConnection(s);
    }

    public static NetClient connect(String host, int port, int timeoutMs) throws IOException {
        Socket s = new Socket();
        s.connect(new InetSocketAddress(host, port), timeoutMs);
        // Do not auto-disconnect on long idle periods (lobby selection / after result).
        s.setSoTimeout(0);
        return new NetClient(s, host, port, timeoutMs);
    }

    public void startReader(ProtocolListener l) {
        reader = new Thread(() -> {
            boolean okSeen = false;
            while (!closing) {
                try {
                    String line = br.readLine();
                    if (line == null) throw new EOFException("server closed");
                    String t = line.trim();
                    if (t.isEmpty()) continue;

                    if (t.startsWith("C45PING")) {
                        try { sendPong(); } catch (IOException ignored) {}
                        continue;
                    }
                    if (t.startsWith("C45RECONNECT_OK")) {
                        continue;
                    }
                    if (t.startsWith("C45OPPDOWN")) {
                        String[] p = t.split("\\s+");
                        String who = (p.length >= 2) ? p[1] : "Enemy";
                        String sec = (p.length >= 3) ? p[2] : "30";
                        l.onResult(who + " disconnected. Waiting up to " + sec + " seconds...");
                        continue;
                    }
                    if (t.startsWith("C45OPPBACK")) {
                        String[] p = t.split("\\s+");
                        String who = (p.length >= 2) ? p[1] : "Enemy";
                        l.onResult(who + " reconnected. Continuing the game.");
                        continue;
                    }

                    // C45LL... parsing
//                    String framed = null;
//                    try { framed = tryParseC45Frame(t); }
//                    catch (IOException bad) { l.onServerError(bad.getMessage()); break; }
//
//                    if (framed != null) {
//                        t = coerceToLegacyToken(framed);
//                    }

                    // базовые токены
                    if (t.startsWith("C45OK")) {
                        if (!okSeen) { okSeen = true; l.onOk(); }
                        else { l.onLobbyJoinOk(); }

                        continue;
                    }
                    if(t.startsWith("C45WRONG NAME_TAKEN")){
                        l.onServerError("Name has been taken");
                        break;
                    }
                    if (t.startsWith("C45WRONG") || t.startsWith("WRONG")) {
                        l.onServerError("WRONG");
                        continue;
                    }

                    // снимок лобби: "C45LOBBIES N" + N строк "C45LOBBY ..."
                    if (t.startsWith("C45LOBBIES")) {
                        String[] p = t.split("\\s+");
                        int n = (p.length >= 2) ? Integer.parseInt(p[1]) : Integer.parseInt(br.readLine().trim());
                        List<LobbyRow> rows = new ArrayList<>();
                        for (int i = 0; i < n; i++) rows.add(parseLobbyLine(br.readLine().trim()));
                        l.onLobbySnapshot(rows);
                        continue;
                    }

                    // игровая часть
                    if (t.startsWith("C45DEAL")) {
                        String[] p = t.split("\\s+");
                        l.onDeal(p[1], p[2]);
                        continue;
                    }
                    if (t.startsWith("C45TURN")) {
                        String[] p = t.split("\\s+");
                        String who = (p.length >= 2) ? p[1] : "?";
                        int sec = (p.length >= 3) ? Integer.parseInt(p[2]) : 30;
                        l.onTurn(who, sec); continue;
                    }
                    if (t.startsWith("C45CARD")) {
                        String[] p = t.split("\\s+");
                        if (p.length >= 2) l.onCard(p[1]); continue;
                    }
                    if (t.startsWith("C45BUST")) {
                        String[] p = t.split("\\s+");
                        if (p.length >= 3) l.onBust(p[1], Integer.parseInt(p[2])); continue;
                    }
                    if (t.startsWith("C45RESULT")) {
                        String data = t.substring("C45RESULT".length()).trim();
                        String[] parts = data.split(" ");
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
                        continue;
                    }
                    if (t.startsWith("C45WAITING")) {
                        try {
                            sendYes();
                        } catch (IOException ioe) {
                            l.onServerError("send C45YES failed: " + ioe.getMessage());
                        }
                    }
                } catch (Exception ex) {
                    if (closing) return;
                    if (tryReconnect()) continue;
                    System.out.println(ex);
                    System.out.println("Trying read Bad line");
                    if (lastName != null && !lastName.isBlank() && lastLobby > 0) {
                        l.onServerError("Unable to restore connection within 30 seconds. Please reconnect manually.");
                    } else {
                        l.onServerError(ex.getMessage());
                    }
                    return;
                }
            }
        }, "net-reader");
        reader.setDaemon(true);
        reader.start();
    }

    // отправка команд
    public synchronized void sendRaw(String s) throws IOException { os.write(s.getBytes(StandardCharsets.UTF_8)); os.flush(); }
    public void sendName(String name) throws IOException {
        lastName = name;
        sendRaw("C45" + name + "\n");
    }
    // Совместимо с твоим текущим форматом "C45<name><lobby>"
    public void sendJoin(String name, int lobby) throws IOException {
        lastName = name;
        lastLobby = lobby;
        sendRaw("C45" + name + lobby + "\n");
        System.out.println(lobby);
    }
    // Если перейдёшь на явную команду: sendRaw("C45JOIN " + lobby + "\n");
    public void sendHit() throws IOException { sendRaw("C45HIT\n"); }
    public void sendStand() throws IOException { sendRaw("C45STAND\n"); }
    public void sendYes() throws IOException { sendRaw("C45YES\n"); }
    public void sendPong() throws IOException { sendRaw("C45PONG\n"); }
    public void sendBackToLobby(String name) throws IOException { sendRaw("C45" + (name == null ? "" : name) + "back\n"); }

//    public void sendName(String name) throws IOException { sendRaw(buildC45Frame(name)); }
//    // "C45<name><lobby>" как и было — но в payload
//    public void sendJoin(String name, int lobby) throws IOException { sendRaw(buildC45Frame(name + lobby)); }
//    public void sendHit() throws IOException { sendRaw(buildC45Frame("HIT")); }
//    public void sendStand() throws IOException { sendRaw(buildC45Frame("STAND")); }
//    public void sendYes() throws IOException { sendRaw(buildC45Frame("YES")); }

    public void closeQuietly() {
        // Close socket first to unblock reader thread; closing BufferedReader can block on its internal lock.
        closing = true;
        try { socket.close(); } catch (Exception ignored) {}
        try { if (reader != null) reader.interrupt(); } catch (Exception ignored) {}
    }

    private synchronized void replaceConnection(Socket s) throws IOException {
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        this.socket = s;
        this.os = s.getOutputStream();
        this.br = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
    }

    private boolean tryReconnect() {
        String n = lastName;
        int lobby = lastLobby;
        if (n == null || n.isBlank() || lobby <= 0) return false;

        long deadline = System.currentTimeMillis() + 30000L;
        while (!closing && System.currentTimeMillis() < deadline) {
            try {
                Socket s = new Socket();
                int to = Math.min(connectTimeoutMs, 3000);
                s.connect(new InetSocketAddress(host, port), to);
                s.setSoTimeout(0);
                replaceConnection(s);
                sendRaw("C45RECONNECT " + n + " " + lobby + "\n");
                return true;
            } catch (Exception ignored) {
                try { Thread.sleep(500); } catch (InterruptedException ie) { break; }
            }
        }

        return false;
    }



    // перенос парсера строки лобби из MainApp
    private LobbyRow parseLobbyLine(String line) throws IOException {
        String[] parts = line.split("\\s+");
        if (parts.length != 4 || !parts[0].equalsIgnoreCase("C45LOBBY"))
            throw new IOException("Bad lobby line: " + line);
        int id = Integer.parseInt(parts[1]);
        String[] ps = parts[2].split("=")[1].split("/");
        int players = Integer.parseInt(ps[0]);
        int cap = Integer.parseInt(ps[1]);
        String status = parts[3].split("=")[1];
        return new LobbyRow(id, players, cap, status);
    }

    // --- C45 framed messages ---

    private static String buildC45Frame(String payload) {
        if (payload == null) payload = "";
        int len = payload.length();
        if (len > 99) throw new IllegalArgumentException("C45 payload too long: " + len);
        return String.format("C45%02d%s\n", len, payload);
    }

    /** Возвращает payload, если строка — C45-кадр; иначе null. Бросает, если плохая длина. */
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

    /** Совместимость: если кадр разобран — превратим в старый вид "C45..." (добавим префикс). */
    private static String coerceToLegacyToken(String maybePayloadOrNull) {
        if (maybePayloadOrNull == null) return null;
        return "C45" + maybePayloadOrNull;
    }

}
