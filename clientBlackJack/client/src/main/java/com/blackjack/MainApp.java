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

/**
 * MainApp
 *
 * Purpose:
 *   JavaFX entry point for the Blackjack client UI.
 *
 * Responsibilities:
 *   - Render connection/name/lobby/game screens.
 *   - Wire UI actions to {@link com.blackjack.net.NetClient} commands.
 *   - React to protocol events via {@link com.blackjack.net.ProtocolListener}.
 *
 * Table of contents:
 *   - Scene builders: buildConnectScene(), buildNameScene(), buildLobbyChoiceScene(), buildLobbyWaitingScene()
 *   - Protocol listener wiring: buildListener()
 *   - Navigation helpers and lifecycle: goToConnectScene(), closeClient(), stop()
 */
public class MainApp extends Application {

    /* --- Network/session state --- */
    private NetClient client;
    private String name;
    private int lastLobbySelected = -1;
    private String lastHost;
    private int lastPort;

    /* --- UI state --- */
    private Stage primaryStage;
    private GameView gameView; // created when entering a lobby

    /**
     * JavaFX lifecycle entry point.
     *
     * @param stage Primary stage.
     */
    @Override
    public void start(Stage stage) {
        this.primaryStage = stage;
        stage.setTitle("Blackjack Client — connection");
        stage.setScene(buildConnectScene(null));
        stage.show();
    }

    /* --- Lobby row model (used by NetClient) --- */
    public static class LobbyRow {
        private final IntegerProperty id = new SimpleIntegerProperty();
        private final IntegerProperty players = new SimpleIntegerProperty();
        private final IntegerProperty capacity = new SimpleIntegerProperty();
        private final StringProperty status = new SimpleStringProperty();

        /**
         * Create a lobby row model.
         *
         * @param id       Lobby id (1-based).
         * @param players  Current player count.
         * @param capacity Lobby capacity.
         * @param status   Lobby status string from the server.
         */
        public LobbyRow(int id, int players, int capacity, String status) {
            this.id.set(id);
            this.players.set(players);
            this.capacity.set(capacity);
            this.status.set(status);
        }

        /** @return Lobby id (1-based). */
        public int getId() { return id.get(); }
        /** @return Current player count. */
        public int getPlayers() { return players.get(); }
        /** @return Lobby capacity. */
        public int getCapacity() { return capacity.get(); }
        /** @return Lobby status string from the server. */
        public String getStatus() { return status.get(); }
    }

    // =================================================================
    //                                SCENES
    // =================================================================

    /**
     * Build the initial connection scene (enter IP/port).
     *
     * @param msg Optional status message shown under the buttons.
     * @return JavaFX scene.
     */
    private Scene buildConnectScene(String msg) {
        TextField ipField = new TextField();
        ipField.setPromptText("Server IP (for example: 127.0.0.1)");
        TextField portField = new TextField();
        portField.setPromptText("Server port (for example: 10000)");

        Button connectBtn = new Button("Connect");
        Button reconnectBtn = new Button("Reconnect");
        Label status = new Label(msg == null ? "" : msg);

        if (lastHost != null && !lastHost.isBlank()) ipField.setText(lastHost);
        if (lastPort > 0) portField.setText(Integer.toString(lastPort));

        reconnectBtn.setDisable(!canManualReconnect());

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

            lastHost = ip;
            lastPort = port;

            connectBtn.setDisable(true);
            status.setText("Connection...");

            new Thread(() -> {
                try {
                    closeClient();
                    client = NetClient.connect(ip, port, 10000);
                    client.startReader(buildListener());
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

        reconnectBtn.setOnAction(e -> {
            if (!canManualReconnect()) {
                status.setText("Nothing to reconnect: join a lobby first.");
                return;
            }

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

            lastHost = ip;
            lastPort = port;

            reconnectBtn.setDisable(true);
            connectBtn.setDisable(true);
            status.setText("Reconnecting...");

            new Thread(() -> {
                try {
                    closeClient();
                    client = NetClient.connect(ip, port, 5000);
                    client.startReader(buildListener());
                    client.sendReconnect(name, lastLobbySelected);
                    Platform.runLater(() -> {
                        primaryStage.setTitle("Blackjack Client — reconnecting");
                        primaryStage.setScene(buildLobbyWaitingScene("Reconnecting to the server..."));
                    });
                } catch (Exception ex2) {
                    String reason = normalizeNetError(ex2);
                    goToConnectScene("Connection ERROR: " + reason);
                } finally {
                    Platform.runLater(() -> {
                        reconnectBtn.setDisable(false);
                        connectBtn.setDisable(false);
                    });
                }
            }, "reconnect-thread").start();
        });

        GridPane root = new GridPane();
        root.setPadding(new Insets(16));
        root.setVgap(10);
        root.setHgap(10);
        root.add(new Label("Server IP:"), 0, 0);
        root.add(ipField, 1, 0);
        root.add(new Label("Port:"), 0, 1);
        root.add(portField, 1, 1);
        root.add(new HBox(8, connectBtn, reconnectBtn), 1, 2);
        root.add(status, 1, 3);
        GridPane.setColumnSpan(status, 2);
        root.setAlignment(Pos.CENTER);

        return new Scene(root, 460, 200);
    }

    /**
     * Build the "enter nickname" scene.
     *
     * @return JavaFX scene.
     */
    private Scene buildNameScene() {
        TextField nameField = new TextField();
        nameField.setPromptText("Your name");
        // Protocol uses whitespace-delimited tokens, so names must not contain spaces/tabs/newlines.
        nameField.setTextFormatter(new TextFormatter<>(change -> {
            String t = change.getText();
            if (t != null && t.chars().anyMatch(Character::isWhitespace)) return null;
            return change;
        }));
        Button sendBtn = new Button("Send");
        Label status = new Label();

        sendBtn.setOnAction(e -> {
            String candidate = nameField.getText().trim();
            if (candidate.isEmpty()) { status.setText("Name cannot be empty."); return; }
            if (candidate.chars().anyMatch(Character::isWhitespace)) {
                status.setText("Name cannot contain spaces.");
                return;
            }
            if (candidate.length() > 63) {
                status.setText("Name is too long (max 63 chars).");
                return;
            }
            name = candidate;
            sendBtn.setDisable(true);
            status.setText("Sending name...");

            new Thread(() -> {
                try {
                    if (client == null) throw new SocketException("Connection broke");
                    // Reader thread is started on connect; here we only send the name handshake.
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

    /**
     * Build a waiting scene shown while the client expects a lobby snapshot.
     *
     * @return JavaFX scene.
     */
    private Scene buildLobbyWaitingScene() {
        return buildLobbyWaitingScene("Confirmed. Waiting for the lobby list from the server...");
    }

    /**
     * Build a waiting scene with a custom message.
     *
     * @param message Message shown above the progress indicator.
     * @return JavaFX scene.
     */
    private Scene buildLobbyWaitingScene(String message) {
        Label lbl = new Label(message == null ? "" : message);
        ProgressIndicator pi = new ProgressIndicator();
        pi.setPrefSize(48, 48);

        VBox mid = new VBox(12, lbl, pi);
        mid.setAlignment(Pos.CENTER);

        VBox root = new VBox(20, mid);
        root.setPadding(new Insets(16));
        root.setAlignment(Pos.CENTER);

        return new Scene(root, 520, 260);
    }

    /**
     * Build the lobby selection scene.
     *
     * @param rows Lobby rows received from the server.
     * @return JavaFX scene.
     */
    private Scene buildLobbyChoiceScene(ObservableList<LobbyRow> rows) {
        VBox listBox = new VBox(8);
        listBox.setFillWidth(true);
        int maxLobby = rows.stream().mapToInt(LobbyRow::getId).max().orElse(0);

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
        lobbyField.setPromptText("Enter lobby number(1-" + maxLobby + ")");

        Label status = new Label();
        Button joinBtn = new Button("Connect to lobby");

        joinBtn.setOnAction(e -> {
            String t = lobbyField.getText().trim();
            int num;
            try {
                num = Integer.parseInt(t);
                if (num < 1 || num > maxLobby) throw new NumberFormatException();
            } catch (NumberFormatException ex) {
                status.setText("Enter number from 1 to " + maxLobby + ".");
                return;
            }

            joinBtn.setDisable(true);
            status.setText("Sending lobby number...");

            lastLobbySelected = num;

            new Thread(() -> {
                try {
                    if (client == null) throw new SocketException("Connection broke");
                    // Compatible with the current server: "C45" + name + lobbyNum + "\n"
                    client.sendJoin(Objects.requireNonNullElse(name, ""), num);
                    Platform.runLater(() -> status.setText("Send: " + num));
                    // Join confirmation is delivered via onLobbyJoinOk() from the protocol listener.
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
            resetSession();
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
    //                          PROTOCOL LISTENER
    // =================================================================

    /**
     * Build a {@link ProtocolListener} that updates the UI based on server messages.
     *
     * @return Listener instance.
     */
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

            // --- game phase ---
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
    //                           UTILITIES / LIFECYCLE
    // =================================================================

    /**
     * Lazily create the {@link GameView} if it has not been created yet.
     */
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

    /**
     * Send a "back to lobby" request and switch the UI into the waiting state.
     */
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

    /**
     * Navigate back to the connect scene and close the current client connection.
     *
     * @param message Status message to show on the connect scene.
     */
    private void goToConnectScene(String message) {
        closeClient();
        Platform.runLater(() -> {
            primaryStage.setTitle("Blackjack Client — connecting");
            primaryStage.setScene(buildConnectScene(message));
        });
    }

    /**
     * Convert low-level network exceptions into user-friendly messages.
     *
     * @param ex Exception instance.
     * @return Normalized message string.
     */
    private String normalizeNetError(Exception ex) {
        String s = ex.getMessage();
        if (s == null || s.isBlank()) s = ex.getClass().getSimpleName();
        if (ex instanceof EOFException) s = "Connection closed by server";
        return s;
    }

    /**
     * Close the current {@link NetClient} (best effort) and clear UI state that depends on it.
     */
    private void closeClient() {
        try { if (client != null) client.closeQuietly(); } catch (Exception ignored) {}
        client = null;
        gameView = null;
    }

    /**
     * Reset session state (nickname and last lobby selection).
     */
    private void resetSession() {
        name = null;
        lastLobbySelected = -1;
    }

    /**
     * @return true if the session has enough state to attempt manual reconnect.
     */
    private boolean canManualReconnect() {
        return name != null && !name.isBlank() && lastLobbySelected > 0;
    }

    /**
     * JavaFX lifecycle callback: called on application shutdown.
     */
    @Override
    public void stop() {
        closeClient();
    }

    /**
     * Main entry point for launching the JavaFX application.
     *
     * @param args CLI args.
     */
    public static void main(String[] args) {
        Application.launch(MainApp.class, args);
    }
}
