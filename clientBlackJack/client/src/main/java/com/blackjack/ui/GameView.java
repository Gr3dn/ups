package com.blackjack.ui;

import com.blackjack.net.NetClient;
import java.util.ArrayList;
import java.util.List;
import javafx.application.Platform;
import javafx.geometry.Insets;
import javafx.scene.Scene;
import javafx.scene.control.*;
import javafx.scene.layout.*;

/**
 * GameView
 *
 * Purpose:
 *   Simple JavaFX view for an in-lobby Blackjack match.
 *
 * Responsibilities:
 *   - Render current hand and log of events.
 *   - Enable/disable HIT/STAND based on turn ownership.
 *   - Provide a "Back to Lobby" action when the game ends.
 *
 * Table of contents:
 *   - Lifecycle: GameView(), scene(), bindClient()
 *   - Protocol callbacks: onDeal(), onTurn(), onCard(), onBust(), onOpponentDisconnected(), onOpponentReconnected(), onResult()
 *   - UI helpers: setTurnEnabled(), setBackEnabled(), append()
 *   - Hand helpers: addCardToHand(), updateScore(), handValue()
 */
public class GameView {
    private final VBox root = new VBox(10);
    private final Label header = new Label();
    private final Label reconnectStatus = new Label();
    private final Label scoreLabel = new Label();
    private final TextArea log = new TextArea();
    private final Button hit = new Button("HIT");
    private final Button stand = new Button("STAND");
    private final Button backToLobby = new Button("Back to Lobby");
    private NetClient client;
    private Runnable onBackToLobby;
    private String myName;
    private final List<String> hand = new ArrayList<>();
    private boolean matchEnded = false;
    private boolean restoringHandSnapshot = false;

    /**
     * Create a game view for a given lobby number.
     *
     * @param lobbyNum 1-based lobby number (used only for the header text).
     */
    public GameView(int lobbyNum) {
        header.setStyle("-fx-font-size:18px;-fx-font-weight:bold;");
        header.setText("Lobby #" + lobbyNum + " — Game");
        reconnectStatus.setStyle("-fx-text-fill:#b00020;-fx-font-weight:bold;");
        reconnectStatus.setVisible(false);
        reconnectStatus.setManaged(false);
        scoreLabel.setStyle("-fx-font-size:18px;");
        scoreLabel.setText("Total score: 0");
        log.setEditable(false); log.setPrefRowCount(14);
        HBox actions = new HBox(8, hit, stand, backToLobby);
        root.getChildren().addAll(header, reconnectStatus, scoreLabel, log, actions);
        root.setPadding(new Insets(12));

        hit.setOnAction(e -> { setTurnEnabled(false); sendAsync(() -> client.sendHit()); });
        stand.setOnAction(e -> { setTurnEnabled(false); sendAsync(() -> client.sendStand()); });
        backToLobby.setOnAction(e -> { if (onBackToLobby != null) onBackToLobby.run(); });
        setTurnEnabled(false);
        setBackEnabled(false);
    }

    /**
     * @return A new JavaFX scene containing this view.
     */
    public Scene scene() { return new Scene(root, 640, 420); }

    /**
     * Bind the view to a {@link NetClient} used to send game commands.
     *
     * @param c NetClient instance.
     */
    public void bindClient(NetClient c) { this.client = c; }

    /**
     * Show a connection/reconnect status text inside the game view.
     *
     * Must be called from the JavaFX thread.
     *
     * @param text Status text (null is treated as empty).
     */
    public void showReconnectStatus(String text) {
        reconnectStatus.setText(text == null ? "" : text);
        reconnectStatus.setVisible(true);
        reconnectStatus.setManaged(true);
    }

    /**
     * Hide the connection/reconnect status text.
     *
     * Must be called from the JavaFX thread.
     */
    public void hideReconnectStatus() {
        reconnectStatus.setText("");
        reconnectStatus.setVisible(false);
        reconnectStatus.setManaged(false);
    }

    /**
     * Set callback invoked when user clicks "Back to Lobby".
     *
     * @param r Runnable callback.
     */
    public void setOnBackToLobby(Runnable r) { this.onBackToLobby = r; }

    /**
     * Set the local player name used to decide turn ownership.
     *
     * @param name Player name.
     */
    public void setMyName(String name) { this.myName = name; }

    /**
     * Handle initial two-card deal.
     *
     * @param c1 First card.
     * @param c2 Second card.
     */
    public void onDeal(String c1, String c2){
        boolean isSnapshot = !matchEnded && !hand.isEmpty();
        matchEnded = false;
        restoringHandSnapshot = isSnapshot;
        setBackEnabled(false);
        backToLobby.setText("Back to Lobby");
        hand.clear();
        addCardToHand(c1);
        addCardToHand(c2);
        updateScore();
        if (!isSnapshot) {
            append("Your cards: " + formatCard(c1) + " " + formatCard(c2));
        }
    }

    /**
     * Handle turn notification.
     *
     * @param who Player name whose turn it is.
     * @param sec Turn timeout in seconds.
     */
    public void onTurn(String who, int sec){
        if (restoringHandSnapshot) {
            append("Reconnected. Game state restored.");
            restoringHandSnapshot = false;
        }
        append("Move: " + who + " (" + sec + "s)");
        if (!matchEnded) {
            // If we previously offered "Leave Lobby" while the opponent was disconnected,
            // hide it once the game continues (a new turn indicates the match is running again).
            setBackEnabled(false);
            backToLobby.setText("Back to Lobby");
        }
        setTurnEnabled(myName != null && myName.equals(who));
    }

    /**
     * Handle a newly received card for the local player.
     *
     * @param c Card string.
     */
    public void onCard(String c){
        addCardToHand(c);
        updateScore();
        if (!restoringHandSnapshot) {
            append("You take: " + formatCard(c));
        }
    }

    /**
     * Handle a bust notification.
     *
     * @param p Player name.
     * @param v Hand value.
     */
    public void onBust(String p, int v){
        boolean isMe = myName != null && myName.equals(p);
        if (isMe) {
            scoreLabel.setText("Total score: " + v);
            append(p + " OverTake (" + v + ")");
        }
        setTurnEnabled(false);
    }

    /**
     * Handle notification that the opponent disconnected during the match.
     *
     * The match is paused on the server for a limited time, so we offer a way
     * to leave the lobby and return to the lobby list.
     *
     * @param opponent Opponent name.
     * @param seconds  Max time server waits for reconnect.
     */
    public void onOpponentDisconnected(String opponent, int seconds) {
        matchEnded = false;
        setTurnEnabled(false);
        backToLobby.setText("Leave Lobby");
        setBackEnabled(true);
        String who = (opponent == null || opponent.isBlank()) ? "Opponent" : opponent;
        append(who + " disconnected. Waiting up to " + seconds + " seconds...");
    }

    /**
     * Handle notification that the opponent reconnected and the match continues.
     *
     * @param opponent Opponent name.
     */
    public void onOpponentReconnected(String opponent) {
        matchEnded = false;
        String who = (opponent == null || opponent.isBlank()) ? "Opponent" : opponent;
        append(who + " reconnected. Continuing the game.");
        backToLobby.setText("Back to Lobby");
        setBackEnabled(false);
    }

    /**
     * Handle match result notification.
     *
     * @param s Summary string.
     */
    public void onResult(String s){
        matchEnded = true;
        restoringHandSnapshot = false;
        System.out.println(s);
        append("Result: " + s);
        setTurnEnabled(false);
        backToLobby.setText("Back to Lobby");
        setBackEnabled(true);
    }

    /**
     * Enable or disable the HIT/STAND buttons.
     *
     * @param b true to enable; false to disable.
     */
    private void setTurnEnabled(boolean b){ hit.setDisable(!b); stand.setDisable(!b); }

    /**
     * Show or hide the "Back to Lobby" button.
     *
     * @param b true to show; false to hide.
     */
    private void setBackEnabled(boolean b){
        backToLobby.setDisable(!b);
        backToLobby.setVisible(b);
        backToLobby.setManaged(b);
    }

    /**
     * Append one line to the log.
     *
     * @param s Text to append.
     */
    private void append(String s){ log.appendText((log.getText().isEmpty()?"":"\n") + s); }

    /**
     * Send a command asynchronously to avoid blocking the JavaFX UI thread.
     *
     * @param io Operation that performs the send.
     */
    private void sendAsync(IO io){
        Thread t = new Thread(() -> {
            try {
                io.run();
            } catch (Exception ex) {
                Platform.runLater(() -> append("Error sending: " + ex.getMessage()));
            }
        }, "send-game-cmd");
        t.setDaemon(true);
        t.start();
    }

    /**
     * Functional interface for a send operation that may throw.
     */
    private interface IO { void run() throws Exception; }

    /**
     * Add a card to the local hand list after basic validation/trim.
     *
     * @param card Card string.
     */
    private void addCardToHand(String card) {
        if (card == null) return;
        String c = card.trim();
        if (c.isEmpty()) return;
        hand.add(c);
    }

    /**
     * Format a compact card string (e.g. "AS") into a user-friendly string.
     *
     * @param card Raw card string.
     * @return Formatted card string.
     */
    private static String formatCard(String card) {
        if (card == null) return "";
        String c = card.trim();
        if (c.length() < 2) return c;

        char rank = Character.toUpperCase(c.charAt(0));
        char suit = Character.toUpperCase(c.charAt(1));
        String symbol = switch (suit) {
            case 'D' -> "♦️";
            case 'H' -> "♥️";
            case 'S' -> "♠️";
            case 'C' -> "♣️";
            default -> String.valueOf(c.charAt(1));
        };

        return String.valueOf(rank) + symbol;
    }

    /**
     * Update the score label based on current hand.
     */
    private void updateScore() {
        scoreLabel.setText("Total score: " + handValue(hand));
    }

    /**
     * Compute Blackjack hand value from compact card strings.
     *
     * @param cards Card strings (e.g. "AS", "TD").
     * @return Hand value.
     */
    private static int handValue(List<String> cards) {
        int sum = 0;
        int aces = 0;
        for (String c : cards) {
            if (c == null || c.isEmpty()) continue;
            char r = Character.toUpperCase(c.charAt(0));
            if (r == 'A') {
                aces++;
                sum += 11;
            } else if (r >= '2' && r <= '9') {
                sum += (r - '0');
            } else if (r == 'T' || r == 'J' || r == 'Q' || r == 'K') {
                sum += 10;
            }
        }
        while (sum > 21 && aces > 0) {
            sum -= 10;
            aces--;
        }
        return sum;
    }
}
