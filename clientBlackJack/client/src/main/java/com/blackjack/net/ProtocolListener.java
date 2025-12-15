package com.blackjack.net;

import com.blackjack.MainApp.LobbyRow;
import java.util.List;

public interface ProtocolListener {
    // подключение и лобби
    default void onOk(){ }
    default void onLobbySnapshot(List<LobbyRow> rows){ }
    default void onLobbyJoinOk(){ }
    default void onServerError(String msg){ }

    // игровая фаза
    default void onDeal(String c1, String c2){ }
    default void onTurn(String player, int seconds){ }
    default void onCard(String card){ }
    default void onBust(String player, int value){ }
    default void onResult(String summary){ }
//    default void onWaitingPing(){ }

}
