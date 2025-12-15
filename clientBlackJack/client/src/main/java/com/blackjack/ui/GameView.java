package com.blackjack.ui;

import com.blackjack.net.NetClient;
import javafx.geometry.Insets;
import javafx.scene.Scene;
import javafx.scene.control.*;
import javafx.scene.layout.*;

public class GameView {
    private final VBox root = new VBox(10);
    private final Label header = new Label();
    private final TextArea log = new TextArea();
    private final Button hit = new Button("HIT");
    private final Button stand = new Button("STAND");
    private NetClient client;

    public GameView(int lobbyNum) {
        header.setStyle("-fx-font-size:18px;-fx-font-weight:bold;");
        header.setText("Lobby #" + lobbyNum + " — Game");
        log.setEditable(false); log.setPrefRowCount(14);
        HBox actions = new HBox(8, hit, stand);
        root.getChildren().addAll(header, log, actions);
        root.setPadding(new Insets(12));

        hit.setOnAction(e -> safe(() -> client.sendHit()));
        stand.setOnAction(e -> safe(() -> client.sendStand()));
        setTurnEnabled(false);
    }

    public Scene scene() { return new Scene(root, 640, 420); }
    public void bindClient(NetClient c) { this.client = c; }

    public void onDeal(String c1, String c2){ append("Your cards: " + c1 + " " + c2);}
    public void onTurn(String who, int sec){ append("Move: " + who + " (" + sec + "s)"); setTurnEnabled(true); }
    public void onCard(String c){ append("You take: " + c); }
    public void onBust(String p, int v){ append(p + " OverTake (" + v + ")"); setTurnEnabled(false); }
    public void onResult(String s){ System.out.println(s);append("Result: " + s); setTurnEnabled(false); }

    private void setTurnEnabled(boolean b){ hit.setDisable(!b); stand.setDisable(!b); }
    private void append(String s){ log.appendText((log.getText().isEmpty()?"":"\n") + s); }
    private void safe(IO io){ try { io.run(); } catch(Exception ex){ append("Ошибка отправки: " + ex.getMessage()); } }
    private interface IO { void run() throws Exception; }
}
