package com.blackjack.ui;

import com.blackjack.net.NetClient;
import java.util.ArrayList;
import java.util.List;
import javafx.geometry.Insets;
import javafx.scene.Scene;
import javafx.scene.control.*;
import javafx.scene.layout.*;

public class GameView {
    private final VBox root = new VBox(10);
    private final Label header = new Label();
    private final Label scoreLabel = new Label();
    private final TextArea log = new TextArea();
    private final Button hit = new Button("HIT");
    private final Button stand = new Button("STAND");
    private final Button backToLobby = new Button("Back to Lobby");
    private NetClient client;
    private Runnable onBackToLobby;
    private String myName;
    private final List<String> hand = new ArrayList<>();

    public GameView(int lobbyNum) {
        header.setStyle("-fx-font-size:18px;-fx-font-weight:bold;");
        header.setText("Lobby #" + lobbyNum + " — Game");
        scoreLabel.setStyle("-fx-font-size:14px;");
        scoreLabel.setText("Total score: 0");
        log.setEditable(false); log.setPrefRowCount(14);
        HBox actions = new HBox(8, hit, stand, backToLobby);
        root.getChildren().addAll(header, scoreLabel, log, actions);
        root.setPadding(new Insets(12));

        hit.setOnAction(e -> { setTurnEnabled(false); safe(() -> client.sendHit()); });
        stand.setOnAction(e -> { setTurnEnabled(false); safe(() -> client.sendStand()); });
        backToLobby.setOnAction(e -> { if (onBackToLobby != null) onBackToLobby.run(); });
        setTurnEnabled(false);
        setBackEnabled(false);
    }

    public Scene scene() { return new Scene(root, 640, 420); }
    public void bindClient(NetClient c) { this.client = c; }
    public void setOnBackToLobby(Runnable r) { this.onBackToLobby = r; }
    public void setMyName(String name) { this.myName = name; }

    public void onDeal(String c1, String c2){
        setBackEnabled(false);
        hand.clear();
        addCardToHand(c1);
        addCardToHand(c2);
        updateScore();
        append("Your cards: " + c1 + " " + c2);
    }
    public void onTurn(String who, int sec){
        append("Move: " + who + " (" + sec + "s)");
        setTurnEnabled(myName != null && myName.equals(who));
    }
    public void onCard(String c){
        addCardToHand(c);
        updateScore();
        append("You take: " + c);
    }
    public void onBust(String p, int v){
        if (myName != null && myName.equals(p)) {
            scoreLabel.setText("Total score: " + v);
        }
        append(p + " OverTake (" + v + ")");
        setTurnEnabled(false);
    }
    public void onResult(String s){ System.out.println(s); append("Result: " + s); setTurnEnabled(false); setBackEnabled(true); }

    private void setTurnEnabled(boolean b){ hit.setDisable(!b); stand.setDisable(!b); }
    private void setBackEnabled(boolean b){
        backToLobby.setDisable(!b);
        backToLobby.setVisible(b);
        backToLobby.setManaged(b);
    }
    private void append(String s){ log.appendText((log.getText().isEmpty()?"":"\n") + s); }
    private void safe(IO io){ try { io.run(); } catch(Exception ex){ append("Error sending: " + ex.getMessage()); } }
    private interface IO { void run() throws Exception; }

    private void addCardToHand(String card) {
        if (card == null) return;
        String c = card.trim();
        if (c.isEmpty()) return;
        hand.add(c);
    }

    private void updateScore() {
        scoreLabel.setText("Total score: " + handValue(hand));
    }

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
