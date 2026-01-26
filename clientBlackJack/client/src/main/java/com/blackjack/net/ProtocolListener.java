package com.blackjack.net;

import com.blackjack.MainApp.LobbyRow;
import java.util.List;

/**
 * ProtocolListener
 *
 * Purpose:
 *   Callback interface used by {@link NetClient} to deliver protocol events to the UI layer.
 *
 * Table of contents:
 *   - Connection and lobby callbacks
 *   - Game-phase callbacks
 */
public interface ProtocolListener {
    /* --- Connection and lobby --- */
    /**
     * Called when the server acknowledges the initial handshake with {@code C45OK}.
     */
    default void onOk(){ }

    /**
     * Called when a full lobby snapshot has been received.
     *
     * @param rows Parsed lobby rows.
     */
    default void onLobbySnapshot(List<LobbyRow> rows){ }

    /**
     * Called when the server confirms a lobby join with {@code C45OK} (second OK).
     */
    default void onLobbyJoinOk(){ }

    /**
     * Called on a protocol error or when the client considers the server unreachable.
     *
     * @param msg Human-readable error message.
     */
    default void onServerError(String msg){ }

    /* --- Game phase --- */
    /**
     * Called when the server deals the initial two cards.
     *
     * @param c1 First card string (e.g. "AS").
     * @param c2 Second card string (e.g. "TD").
     */
    default void onDeal(String c1, String c2){ }

    /**
     * Called at the start of a player's turn.
     *
     * @param player Player name whose turn it is.
     * @param seconds Turn timeout in seconds.
     */
    default void onTurn(String player, int seconds){ }

    /**
     * Called when a new card is drawn for the local player.
     *
     * @param card Card string (e.g. "7H").
     */
    default void onCard(String card){ }

    /**
     * Called when a player busts (exceeds 21).
     *
     * @param player Player name.
     * @param value  Final hand value.
     */
    default void onBust(String player, int value){ }

    /**
     * Called when the opponent disconnects during a running game.
     *
     * The server will keep the match paused for up to {@code seconds} to allow the opponent to reconnect.
     *
     * @param player  Opponent name.
     * @param seconds Max time to wait for reconnect.
     */
    default void onOpponentDisconnected(String player, int seconds){ }

    /**
     * Called when the opponent reconnects and the match continues.
     *
     * @param player Opponent name.
     */
    default void onOpponentReconnected(String player){ }

    /**
     * Called when the match result is received.
     *
     * @param summary Human-readable summary prepared by the client.
     */
    default void onResult(String summary){ }
//    default void onWaitingPing(){ }

}
