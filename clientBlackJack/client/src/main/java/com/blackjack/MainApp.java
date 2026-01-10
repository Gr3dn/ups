package com.blackjack;

import com.blackjack.net.NetClient;
import com.blackjack.net.ProtocolListener;
import com.blackjack.ui.GameView;

import javafx.application.Application;
import javafx.application.Platform;
import javafx.beans.property.*;
import javafx.collections.FXCollections;
import javafx.collections.ObservableList;
import javafx.geometry.Insets;
import javafx.geometry.Pos;
import javafx.scene.Scene;
import javafx.scene.control.*;
import javafx.scene.layout.*;
import javafx.stage.Stage;

import java.io.EOFException;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public class MainApp extends Application {

    // -------- Нетворк / состояние --------
    private NetClient client;
    private String name;
    private int lastLobbySelected = -1;

    // -------- UI --------
    private Stage primaryStage;
    private GameView gameView; // создаём при входе в лобби

    @Override
    public void start(Stage stage) {
        this.primaryStage = stage;
        stage.setTitle("Blackjack Client — connection");
        stage.setScene(buildConnectScene(null));
        stage.show();
    }

    // ---------- Модель строки лобби (используется NetClient) ----------
    public static class LobbyRow {
        private final IntegerProperty id = new SimpleIntegerProperty();
        private final IntegerProperty players = new SimpleIntegerProperty();
        private final IntegerProperty capacity = new SimpleIntegerProperty();
        private final StringProperty status = new SimpleStringProperty();

        public LobbyRow(int id, int players, int capacity, String status) {
            this.id.set(id);
            this.players.set(players);
            this.capacity.set(capacity);
            this.status.set(status);
        }

        public int getId() { return id.get(); }
        public int getPlayers() { return players.get(); }
        public int getCapacity() { return capacity.get(); }
        public String getStatus() { return status.get(); }
    }

    // =================================================================
    //                                СЦЕНЫ
    // =================================================================

    // ---------- Подключение ----------
    private Scene buildConnectScene(String msg) {
        TextField ipField = new TextField();
        ipField.setPromptText("Server IP (for example: 127.0.0.1)");
        TextField portField = new TextField();
        portField.setPromptText("Server port (for example: 10000)");

        Button connectBtn = new Button("Connect");
        Label status = new Label(msg == null ? "" : msg);

        connectBtn.setOnAction(e -> {
            String ip = ipField.getText().trim();
            String portText = portField.getText().trim();
            if (ip.isEmpty() || portText.isEmpty()) { status.setText("Enter IP and Port."); return; }

            int port;
            try {
                port = Integer.parseInt(portText);
                if (port < 1 || port > 65535) throw new NumberFormatException();
            } catch (NumberFormatException ex) {
                status.setText("Port must be a number 1–65535.");
                return;
            }

            connectBtn.setDisable(true);
            status.setText("Connection...");

            new Thread(() -> {
                try {
                    closeClient();
                    client = NetClient.connect(ip, port, 30000);
                    Platform.runLater(() -> {
                        primaryStage.setTitle("Blackjack Client — enter name");
                        primaryStage.setScene(buildNameScene());
                    });
                } catch (Exception ex2) {
                    String reason = normalizeNetError(ex2);
                    goToConnectScene("Connection ERROR: " + reason);
                } finally {
                    Platform.runLater(() -> connectBtn.setDisable(false));
                }
            }, "connect-thread").start();
        });

        GridPane root = new GridPane();
        root.setPadding(new Insets(16));
        root.setVgap(10);
        root.setHgap(10);
        root.add(new Label("Server IP:"), 0, 0);
        root.add(ipField, 1, 0);
        root.add(new Label("Port:"), 0, 1);
        root.add(portField, 1, 1);
        root.add(connectBtn, 1, 2);
        root.add(status, 1, 3);
        GridPane.setColumnSpan(status, 2);
        root.setAlignment(Pos.CENTER);

        return new Scene(root, 460, 200);
    }

    // ---------- Ввод имени ----------
    private Scene buildNameScene() {
        TextField nameField = new TextField();
        nameField.setPromptText("Your name");
        Button sendBtn = new Button("Send");
        Label status = new Label();

        sendBtn.setOnAction(e -> {
            name = nameField.getText().trim();
            if (name.isEmpty()) { status.setText("Name cannot be empty."); return; }
            sendBtn.setDisable(true);
            status.setText("Sending name...");

            new Thread(() -> {
                try {
                    if (client == null) throw new SocketException("Connection broke");
                    // запускаем ридер С ПОДПИСАННЫМ listener'ом
                    client.startReader(buildListener());
                    client.sendName(name);
                } catch (Exception ex) {
                    String reason = normalizeNetError(ex);
                    System.out.println("build Name");
                    goToConnectScene("Connection lost: " + reason);
                } finally {
                    Platform.runLater(() -> sendBtn.setDisable(false));
                }
            }, "send-name-thread").start();
        });

        GridPane root = new GridPane();
        root.setPadding(new Insets(16));
        root.setVgap(10);
        root.setHgap(10);
        root.add(new Label("Name:"), 0, 0);
        root.add(nameField, 1, 0);
        root.add(sendBtn, 1, 1);
        root.add(status, 1, 2);
        GridPane.setColumnSpan(status, 2);
        root.setAlignment(Pos.CENTER);

        return new Scene(root, 420, 160);
    }

    // ---------- Ожидание снимка лобби ----------
    private Scene buildLobbyWaitingScene() {
        Label lbl = new Label("Confirmed. Waiting for the lobby list from the server...");
        ProgressIndicator pi = new ProgressIndicator();
        pi.setPrefSize(48, 48);

        VBox mid = new VBox(12, lbl, pi);
        mid.setAlignment(Pos.CENTER);

        VBox root = new VBox(20, mid);
        root.setPadding(new Insets(16));
        root.setAlignment(Pos.CENTER);

        return new Scene(root, 520, 260);
    }

    // ---------- Выбор лобби ----------
    private Scene buildLobbyChoiceScene(ObservableList<LobbyRow> rows) {
        VBox listBox = new VBox(8);
        listBox.setFillWidth(true);

        Label title = new Label("Available lobbies:");
        title.setStyle("-fx-font-size: 16px; -fx-font-weight: bold;");

        for (LobbyRow r : rows) {
            String text = String.format(
                    "Lobby #%d — players: %d/%d — status: %s",
                    r.getId(), r.getPlayers(), r.getCapacity(), r.getStatus()
            );
            Label item = new Label(text);
            item.setPadding(new Insets(8));
            item.setStyle("-fx-background-color: #f3f3f3; -fx-background-radius: 6;");
            listBox.getChildren().add(item);
        }

        TextField lobbyField = new TextField();
        lobbyField.setPromptText("Enter lobby number(1-5)");

        Label status = new Label();
        Button joinBtn = new Button("Connect to lobby");

        joinBtn.setOnAction(e -> {
            String t = lobbyField.getText().trim();
            int num;
            try {
                num = Integer.parseInt(t);
                if (num < 1 || num > 5) throw new NumberFormatException();
            } catch (NumberFormatException ex) {
                status.setText("Enter number from 1 to 5.");
                return;
            }

            joinBtn.setDisable(true);
            status.setText("Sending lobby number...");

            lastLobbySelected = num;

            new Thread(() -> {
                try {
                    if (client == null) throw new SocketException("Connection broke");
                    // Совместимо с текущим сервером: "C45" + name + lobbyNum + "\n"
                    client.sendJoin(Objects.requireNonNullElse(name, ""), num);
                    Platform.runLater(() -> status.setText("Send: " + num));
                    // Подтверждение входа придёт как onLobbyJoinOk() из listener
                } catch (Exception ex2) {
                    String reason = normalizeNetError(ex2);
                    System.out.println("build Lobby Choice");
                    goToConnectScene("Connection lost: " + reason);
                } finally {
                    Platform.runLater(() -> joinBtn.setDisable(false));
                }
            }, "send-lobby-thread").start();
        });

        Button backBtn = new Button("Disconnect");
        backBtn.setOnAction(e -> {
            closeClient();
            goToConnectScene("Connection closed.");
        });

        VBox root = new VBox(12,
                title,
                listBox,
                new HBox(8, new Label("Lobby number:"), lobbyField, joinBtn),
                status,
                backBtn
        );
        ((HBox)root.getChildren().get(2)).setAlignment(Pos.CENTER_LEFT);
        root.setPadding(new Insets(16));

        return new Scene(root, 560, 400);
    }

    // =================================================================
    //                          LISTENER ПРОТОКОЛА
    // =================================================================

    private ProtocolListener buildListener() {
        return new ProtocolListener() {
            @Override
            public void onOk() {
                Platform.runLater(() -> {
                    primaryStage.setTitle("Blackjack Client — Lobyy (waiting for data)");
                    primaryStage.setScene(buildLobbyWaitingScene());
                });
            }

            @Override
            public void onLobbySnapshot(List<LobbyRow> rows) {
                Platform.runLater(() -> {
                    ObservableList<LobbyRow> data = FXCollections.observableArrayList(new ArrayList<>(rows));
                    primaryStage.setTitle("Blackjack Client — Chose Lobby");
                    primaryStage.setScene(buildLobbyChoiceScene(data));
                });
            }

            @Override
            public void onLobbyJoinOk() {
                Platform.runLater(() -> {
                    int ln = lastLobbySelected > 0 ? lastLobbySelected : 0;
                    gameView = new GameView(ln);
                    gameView.bindClient(client);
                    gameView.setMyName(name);
                    gameView.setOnBackToLobby(() -> MainApp.this.requestBackToLobby());
                    primaryStage.setTitle("Blackjack Client — Game");
                    primaryStage.setScene(gameView.scene());
                });
            }

            @Override
            public void onServerError(String msg) {
                String reason = (msg == null || msg.isBlank()) ? "Server ERROR" : msg;
                System.out.println(msg);
                Platform.runLater(() -> goToConnectScene("Connection lost: " + reason));
            }

//            @Override
//            public void onWaitingPing(){
//                primaryStage.setTitle("... ждём второго игрока ...");
//            }


            // ----- игровая фаза -----
            @Override public void onDeal(String c1, String c2) {
                Platform.runLater(() -> {
                    createGameView();
                    gameView.onDeal(c1, c2);
                });
            }
            @Override public void onTurn(String player, int seconds) {
                Platform.runLater(() -> {
                    createGameView();
                    gameView.onTurn(player, seconds);
                });
            }
            @Override public void onCard(String card) {
                Platform.runLater(() -> {
                    createGameView();
                    gameView.onCard(card);
                });
            }
            @Override public void onBust(String player, int value) {
                Platform.runLater(() -> {
                    createGameView();
                    gameView.onBust(player, value);
                });
            }
            @Override public void onResult(String summary) {
                Platform.runLater(() -> {
                    createGameView();
                    gameView.onResult(summary);
                });
            }
        };
    }

    // =================================================================
    //                           УТИЛИТЫ/ЖИЗНЕННЫЙ ЦИКЛ
    // =================================================================

    private void createGameView(){
        if (gameView != null) return;
        int ln = lastLobbySelected > 0 ? lastLobbySelected : 0;
        gameView = new GameView(ln);
        gameView.bindClient(client);
        gameView.setMyName(name);
        gameView.setOnBackToLobby(() -> MainApp.this.requestBackToLobby());
        primaryStage.setTitle("Blackjack Client — Game");
        primaryStage.setScene(gameView.scene());
    }

    private void requestBackToLobby() {
        Platform.runLater(() -> {
            primaryStage.setTitle("Blackjack Client — Lobby");
            primaryStage.setScene(buildLobbyWaitingScene());
        });
        new Thread(() -> {
            try {
                if (client == null) throw new SocketException("Connection broke");
                client.sendBackToLobby(Objects.requireNonNullElse(name, ""));
            } catch (Exception ex) {
                String reason = normalizeNetError(ex);
                goToConnectScene("Connection lost: " + reason);
            }
        }, "send-back-thread").start();
    }

    private void goToConnectScene(String message) {
        closeClient();
        Platform.runLater(() -> {
            primaryStage.setTitle("Blackjack Client — connecting");
            primaryStage.setScene(buildConnectScene(message));
        });
    }

    private String normalizeNetError(Exception ex) {
        String s = ex.getMessage();
        if (s == null || s.isBlank()) s = ex.getClass().getSimpleName();
        if (ex instanceof EOFException) s = "Connection closed by server";
        return s;
    }

    private void closeClient() {
        try { if (client != null) client.closeQuietly(); } catch (Exception ignored) {}
        client = null;
        gameView = null;
        lastLobbySelected = -1;
    }

    @Override
    public void stop() {
        closeClient();
    }

    public static void main(String[] args) {
        Application.launch(MainApp.class, args);
    }
}
