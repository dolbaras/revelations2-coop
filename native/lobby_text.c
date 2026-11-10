/*
 * Texts of the co-op lobby. Most captions are the game's own strings (the game ships them in all 12 languages; lobby_stock_ids says where),
 * the columns of those entries are only the fallback for a table that is not loaded (and our word where the game's CJK entry is Latin letters only).
 * The own strings are translated here. Every text has all 12 columns (tests/test_lang.c: no language may fall back to English). Column order of every row: English, French, Italian, German, Spanish, Russian, Polish, Brazilian Portuguese,
 * Japanese, Korean, Simplified Chinese, Traditional Chinese (the order of lobby_lang_names). A NULL column falls back to English.
 */
#include <stdio.h>
#include <string.h>

#include "lobby.h"

#define NL NULL

/* message table indexes of Msg_GetString (g_MsgTableNames): 0 m01_sys_common_01, 3 m03_sys_title_topmenu, 6 m09_sys_cockpit, 14 m19_sys_network,
 * 51 m34_sys_raidmatching, 52 m35_sys_raidjoin, 58 m40_sys_popup */
const lobby_stock lobby_stock_ids[LT_COUNT] = {
    [LT_COOP] = {3, 120}, [LT_HOST] = {51, 2}, [LT_JOIN] = {51, 3}, [LT_FRIEND] = {3, 121}, [LT_LINK] = {3, 122}, [LT_BACK] = {3, 77},
    [LT_READY] = {52, 79}, [LT_START] = {3, 302}, [LT_DIFFICULTY] = {3, 107}, [LT_EPISODE] = {3, 151},
    [LT_CLAIRE] = {0, 80}, [LT_MOIRA] = {0, 81}, [LT_BARRY] = {0, 82}, [LT_NATALIA] = {0, 83}, [LT_PLAYER] = {0, 315}, [LT_PARTNER] = {0, 316},
    [LT_YES] = {0, 2}, [LT_NO] = {0, 3}, [LT_QUIT_SESSION] = {58, 116}, [LT_LEAVE] = {6, 120}, [LT_SAME_SEL] = {58, 134},
    [LT_FAIL_JOIN] = {14, 15}, [LT_FAIL_CREATE] = {14, 8}, [LT_CONN_LOST] = {14, 11},
    [LT_CASUAL] = {0, 125}, [LT_NORMAL] = {0, 126}, [LT_HARD] = {0, 180}, [LT_VERY_HARD] = {0, 181}, [LT_INVITE] = {0, 241},
    /* own strings: table -1 */
    [LT_REF_PROTO] = {-1, 0}, [LT_REF_MODVER] = {-1, 0}, [LT_REF_MODVER_OLD] = {-1, 0}, [LT_PARTNER_SILENT] = {-1, 0},
    [LT_WAITING] = {-1, 0}, [LT_PARTNER_JOINED] = {-1, 0}, [LT_ENTER_ADDR] = {-1, 0}, [LT_CONNECTING] = {-1, 0}, [LT_NO_FRIENDS] = {-1, 0},
    [LT_REFRESH] = {-1, 0}, [LT_NAME] = {-1, 0}, [LT_PING] = {-1, 0}, [LT_HINT] = {-1, 0}, [LT_HINT_IP] = {-1, 0}, [LT_YOUR_ADDR] = {-1, 0},
    [LT_STEAM_HINT] = {-1, 0}, [LT_STEAM_NA] = {-1, 0}, [LT_NEED_READY] = {-1, 0}, [LT_NOT_READY] = {-1, 0}, [LT_SEARCHING] = {-1, 0},
    [LT_CHARACTER] = {-1, 0}, [LT_STARTING] = {-1, 0}, [LT_YOU] = {-1, 0}, [LT_HOST_TAG] = {-1, 0}, [LT_PARTNER_READY] = {-1, 0}, [LT_LEAVE_HINT] = {-1, 0}, [LT_SAVE_WARN] = {-1, 0},
    [LT_WAIT_LOAD] = {-1, 0}, [LT_LEG_OK] = {-1, 0}, [LT_DOWNED] = {-1, 0}, [LT_DOWN_SELF] = {-1, 0}, [LT_DOWN_PARTNER] = {-1, 0},
    /* 0.2.0 lobby: stock strings found in all 12 languages (lab\gmd.py scan of m19 / m34 / m35 / m40 / m09 / m03, notes\lobby-must.md 3) */
    [LT_REMOVED] = {14, 39}, [LT_REF_BUILD] = {14, 57}, [LT_KICK_ASK] = {14, 45}, [LT_PARTNER_LEFT_ROOM] = {6, 143}, [LT_PARTNER_LOST_ROOM] = {6, 142},
    [LT_SESSION_OVER] = {58, 76},
    [LT_REF_FULL] = {-1, 0}, [LT_REF_STARTED] = {-1, 0}, [LT_REF_NOANSWER] = {-1, 0}, [LT_REF_BAD] = {-1, 0}, [LT_PARTNER_LEFT] = {-1, 0}, [LT_PARTNER_LOST] = {-1, 0},
    [LT_HOST_ENDED] = {-1, 0}, [LT_HOST_LOST] = {-1, 0}, [LT_EP1_END] = {-1, 0}, [LT_CONNECTING_TO] = {-1, 0}, [LT_AWAITING] = {-1, 0}, [LT_HOST_WAIT_READY] = {-1, 0},
    [LT_HOST_CAN_START] = {-1, 0}, [LT_GUEST_PICK] = {-1, 0}, [LT_GUEST_WAIT_HOST] = {-1, 0}, [LT_KICK] = {-1, 0}, [LT_EP1_ONLY] = {-1, 0}, [LT_HOST_NET_HINT] = {-1, 0},
    [LT_END_COOP] = {-1, 0}, [LT_END_COOP_HELP] = {-1, 0},
    [LT_REF_GONE] = {-1, 0}, [LT_REF_DENIED] = {-1, 0}, [LT_REF_NOLOBBY] = {-1, 0}, [LT_REF_NOTCOOP] = {-1, 0}, [LT_REF_STEAM_NOANSWER] = {-1, 0},
    [LT_MS] = {-1, 0},
    [LT_CONTINUE] = {3, 87}, [LT_CONT_NEED] = {-1, 0},   /* 0.2.1 coopsave: m03 #87 "Continue" (the campaign menu's own row) */
};

const char *const lobby_lang_names[12] = {"English", "French", "Italian", "German", "Spanish", "Russian", "Polish", "Portuguese (Brazil)", "Japanese", "Korean",
                                          "Chinese (Simplified)", "Chinese (Traditional)"};

int lobby_lang_from_game(int g)
{
    switch (g) {
    case 0: return 8;                 /* jpn */
    case 1: return 0;                 /* eng */
    case 2: return 1;                 /* fre */
    case 3: return 4;                 /* spa */
    case 4: return 3;                 /* ger */
    case 5: return 2;                 /* ita */
    case 6: return 9;                 /* kor */
    case 7: return 11;                /* chT */
    case 8: return 10;                /* chS */
    case 9: case 21: return 7;        /* por / ptB */
    case 10: return 5;                /* rus */
    case 11: return 6;                /* pol */
    default: return 0;
    }
}

static const char *const T[LT_COUNT][12] = {
    /* ---- stock strings: the game shows its own words (lobby_stock_ids); these columns are the fallback for a table that is not loaded, in every language
       (lang 2026-11-08: the game's own words of each language, read from the test copy's m01 / m03 / m09 / m19 / m34 / m35 / m40 GMDs; where the game's
       entry of a CJK language is Latin letters only, our word, which menu.c env_text then shows instead of the stock one; the names stay the game's) ---- */
    [LT_COOP] = {"Co-Op Play", "Coop", "Modalità co-op", "Koop-Spiel", "Juego cooperativo", "Взаимодействие", "Tryb Współpracy", "Jogo de Cooperação", "協力プレイ", "협동플레이", "协力游玩", "協力遊玩"},
    [LT_HOST] = {"Find a Partner", "Créer une session", "Crea sessione", "Sitzung erstellen", "Crear una sesión", "Найти партнера", "Znajdź partnera", "Encontrar um parceiro", "パートナーを募集します", "파트너 찾기", "征求同伴", "徵求同伴"},
    [LT_JOIN] = {"Join a Partner", "Rejoindre une session", "Lista sessioni", "Sitzung beitreten", "Unirse a una sesión", "Присоединиться к партнеру", "Dołącz do partnera", "Juntar-se a um parceiro", "パーティーに参加します", "대기실 참가", "加入同伴", "加入同伴"},
    [LT_FRIEND] = {"Friend", "Ami", "Amico", "Freund", "Amigo", "Друг", "Znajomy", "Amigo", "フレンド", "친구", "好友", "好友"},
    [LT_LINK] = {"System Link", "Liaison multiconsole", "System Link", "System-Link", "Conexión de sistema", "Соединение систем", "Łącze systemowe", "Link do sistema", "システムリンク", "시스템 연결", "系统连结", "系統連結"},
    [LT_BACK] = {"Back", "Retour", "Indietro", "Zurück", "Atrás", "Назад", "Powrót", "Voltar", "戻る", "뒤로", "返回", "返回"},
    [LT_READY] = {"Ready", "Prêt", "Pronto", "Bereit", "Listo", "Завершить подготовку", "Zakończ przygotowania", "Pronto", "準備完了", "준비 완료", "准备就绪", "準備就緒"},
    [LT_START] = {"Start Game", "Commencer", "Inizia una partita", "Spiel starten", "Comenzar", "Начать игру", "Rozpocznij grę", "Iniciar jogo", "ゲーム開始", "게임 시작", "开始游戏", "開始遊戲"},
    [LT_DIFFICULTY] = {"Difficulty", "Difficulté", "Difficoltà", "Schwierigkeit", "Dificultad", "Сложность", "Poziom trudności", "Dificuldade", "難易度", "난이도", "难度", "難度"},
    [LT_EPISODE] = {"Episode", "Épisode", "Episodio", "Episode", "Episodio", "Эпизод", "Epizod", "Episódio", "エピソード", "에피소드", "章节", "章節"},
    [LT_CLAIRE] = {"Claire", "Claire", "Claire", "Claire", "Claire", "Клэр", "Claire", "Claire", "CLAIRE", "Claire", "Claire", "Claire"},
    [LT_MOIRA] = {"Moira", "Moira", "Moira", "Moira", "Moira", "Мойра", "Moira", "Moira", "MOIRA", "Moira", "Moira", "Moira"},
    [LT_BARRY] = {"Barry", "Barry", "Barry", "Barry", "Barry", "Барри", "Barry", "Barry", "BARRY", "Barry", "Barry", "Barry"},
    [LT_NATALIA] = {"Natalia", "Natalia", "Natalia", "Natalia", "Natalia", "Наталья", "Natalia", "Natalia", "NATALIA", "Natalia", "Natalia", "Natalia"},
    [LT_PLAYER] = {"Player", "Joueur", "Giocatore", "Spieler", "Jugador", "Игрок", "Gracz", "Jogador", "プレイヤー", "플레이어", "玩家", "玩家"},
    [LT_PARTNER] = {"Partner", "Partenaire", "Partner", "Partner", "Compañero", "Партнер", "Partner", "Parceiro", "パートナー", "파트너", "同伴", "同伴"},
    [LT_YES] = {"Yes", "Oui", "Sì", "Ja", "Sí", "Да", "Tak", "Sim", "はい", "예", "是", "是"},
    [LT_NO] = {"No", "Non", "No", "Nein", "No", "Нет", "Nie", "Não", "いいえ", "아니요", "否", "否"},
    [LT_QUIT_SESSION] = {"Quit session?", "Quitter la session ?", "Abbandonare la sessione?", "Diese Sitzung verlassen?", "¿Quieres abandonar esta sesión?", "Выйти из сеанса?", "Opuścić sesję?", "Sair da sessão?", "セッションから退出しますか？", "세션을 종료하시겠습니까?", "是否要离开游戏？", "是否要離開遊戲？"},
    [LT_LEAVE] = {"Leave", "Quitter", "Abbandona", "Verlassen", "Salir", "Выйти", "Wyjdź", "Abandonar", "退出", "나가기", "离开游戏", "離開遊戲"},
    [LT_SAME_SEL] = {"You cannot make the same selection as your partner.", "Vous ne pouvez pas faire la même sélection que votre partenaire.", "Non puoi scegliere lo stesso personaggio.", "Sie können nicht dasselbe wählen wie Ihr Partner.", "No puedes seleccionar lo mismo que tu compañero.", "Вы не можете выбрать то же самое, что и ваш партнер.", "Nie możesz dokonać tego samego wyboru, co twój partner.", "Você e o seu parceiro não podem fazer a mesma seleção.", "パートナーが選択しているので、同じ項目は選択できません。", "파트너와 동일한 선택을 할 수 없습니다.", "无法和同伴选择相同的项目。", "無法和同伴選擇相同的項目。"},
    [LT_FAIL_JOIN] = {"Failed to join session.", "Impossible de rejoindre la session.", "Impossibile partecipare alla sessione.", "Der Beitritt zur Sitzung ist fehlgeschlagen.", "Fallo al unirse a una sesión.", "Не удалось подключиться к сеансу.", "Nie udało się dołączyć do sesji.", "Falha ao entrar na sessão.", "セッションの参加に失敗しました。", "세션 참가에 실패했습니다.", "加入游戏失败。", "加入遊戲失敗。"},
    [LT_FAIL_CREATE] = {"Failed to create a session.", "Impossible de créer une session.", "Impossibile creare una sessione.", "Es konnte keine Sitzung erstellt werden.", "Fallo al crear una sesión.", "Не удалось создать сеанс.", "Nie udało się stworzyć sesji.", "Falha ao criar uma sessão.", "セッションの作成に失敗しました。", "세션 생성에 실패했습니다.", "建立游戏失败。", "建立遊戲失敗。"},
    [LT_CONN_LOST] = {"Connection to the other player has been terminated.", "Vous avez été déconnecté de l'autre joueur.", "La connessione ad altri giocatori è terminata.", "Die Verbindung zu den anderen Spielern wurde unterbrochen.", "Se ha finalizado la conexión con el otro jugador.", "Подключение к другому игроку прервано.", "Połączenie z innym graczem zostało przerwane.", "A conexão com o outro jogador foi terminada.", "他のプレイヤーとの接続が切断されました。", "다른 플레이어와의 접속이 종료되었습니다.", "与其他玩家建立的连结 已中断。", "與其他玩者建立的連結 已中斷。"},
    [LT_CASUAL] = {"Casual", "Facile", "Facile", "Einfach", "Fácil", "Легкий", "Relaks", "Casual", "カジュアル", "쉬움", "休闲", "休閒"},
    [LT_NORMAL] = {"Normal", "Normal", "Normale", "Normal", "Normal", "Нормальный", "Normalny", "Normal", "ノーマル", "보통", "普通", "普通"},
    [LT_HARD] = {"Hard", "Difficile", "Difficile", "Schwer", "Difícil", "Трудный", "Trudny", "Difícil", "ハード", "어려움", "困难", "困難"},
    [LT_VERY_HARD] = {"Very Hard", "Expert", "Molto difficile", "Sehr Schwer", "Muy difícil", "Очень трудный", "Bardzo trudny", "Muito difícil", "ベリーハード", "매우 어려움", "非常困难", "非常困難"},
    [LT_INVITE] = {"Invite Friends", "Inviter des amis", "Invita amici", "Freunde einladen", "Invitar amigos", "Пригласить друзей", "Zaproś znajomych", "Convidar amigos", "フレンドを招待", "친구 초대", "邀请好友", "邀請好友"},

    /* ---- own strings ---- */
    /* 0.2.0 resilience (our wording, not read by native speakers yet): a refusal because of another co-op package (lobby status line, after "Failed to join
       session."; at most 159 bytes with the versions, tests/test_resil.c), and the strip while the partner's game sends nothing. Order of the %s: fixed. */
    [LT_REF_PROTO] = {"The co-op mod versions do not match (network protocol). Both players need the same version.",
                      "Les versions du mod coop ne correspondent pas (protocole réseau). Il faut la même version des deux côtés.",
                      "Le versioni della mod co-op non corrispondono (protocollo di rete). Serve la stessa versione per entrambi.",
                      "Die Koop-Mod-Versionen passen nicht zusammen (Netzwerkprotokoll). Beide brauchen dieselbe Version.",
                      "Las versiones del mod cooperativo no coinciden (protocolo de red). Ambos necesitan la misma versión.",
                      "Версии кооп-мода не совпадают (сетевой протокол). У обоих нужна одна версия.",
                      "Wersje moda co-op się różnią (protokół sieciowy). Obaj gracze potrzebują tej samej wersji.",
                      "As versões do mod cooperativo não coincidem (protocolo de rede). Os dois precisam da mesma versão.",
                      "協力プレイ MOD のバージョンが違います (通信方式)。二人とも同じバージョンが必要です。",
                      "협동 모드 버전이 서로 다릅니다 (통신 프로토콜). 두 사람 모두 같은 버전이 필요합니다.",
                      "合作模组版本不一致（网络协议）。双方需要相同的版本。", "合作模組版本不一致（網路協定）。雙方需要相同的版本。"},
    [LT_REF_MODVER] = {"Co-op mod versions differ: host %s, you %s. Start the game from the Co-op shortcut to update it.",
                       "Versions du mod coop différentes : hôte %s, vous %s. Lancez le jeu par le raccourci Co-op pour le mettre à jour.",
                       "Versioni della mod co-op diverse: host %s, tu %s. Avvia il gioco dal collegamento Co-op per aggiornarla.",
                       "Koop-Mod-Versionen verschieden: Host %s, Sie %s. Spiel über die Co-op-Verknüpfung starten, das aktualisiert sie.",
                       "Versiones del mod distintas: anfitrión %s, tú %s. Inicia el juego con el acceso directo Co-op para actualizarlo.",
                       "Разные версии кооп-мода: хост %s, вы %s. Запуск ярлыком Co-op обновит мод.",
                       "Różne wersje moda co-op: host %s, ty %s. Uruchom grę skrótem Co-op, aby go zaktualizować.",
                       "Versões do mod diferentes: anfitrião %s, você %s. Abra o jogo pelo atalho Co-op para atualizar.",
                       "協力 MOD バージョン違い: ホスト %s、あなた %s。Co-op ショートカットで起動すると更新されます。",
                       "협동 모드 버전이 다릅니다: 호스트 %s, 나 %s. Co-op 바로 가기로 시작하면 업데이트됩니다.",
                       "合作模组版本不同：主机 %s，你 %s。请通过 Co-op 快捷方式启动游戏以更新。", "合作模組版本不同：主機 %s，你 %s。請透過 Co-op 捷徑啟動遊戲以更新。"},
    [LT_REF_MODVER_OLD] = {"The host has an older co-op mod (you have %s). Start the game from the Co-op shortcut on both PCs to update it.",
                           "L'hôte a un mod coop plus ancien (vous : %s). Lancez le jeu par le raccourci Co-op sur les deux PC.",
                           "L'host ha una mod co-op più vecchia (tu: %s). Avviate il gioco dal collegamento Co-op su entrambi i PC.",
                           "Der Host hat eine ältere Koop-Mod (Sie: %s). Spiel auf beiden PCs über die Co-op-Verknüpfung starten.",
                           "El anfitrión tiene un mod más antiguo (tú: %s). Iniciad el juego con el acceso directo Co-op en ambos PC.",
                           "У хоста старый кооп-мод (у вас %s). Запустите игру ярлыком Co-op на обоих ПК.",
                           "Host ma starszą wersję moda co-op (ty: %s). Uruchomcie grę skrótem Co-op na obu PC.",
                           "O anfitrião tem um mod mais antigo (você: %s). Abram o jogo pelo atalho Co-op nos dois PCs.",
                           "ホストの協力 MOD は古いバージョンです (あなた: %s)。両方の PC で Co-op から起動してください。",
                           "호스트의 협동 모드가 이전 버전입니다 (나: %s). 두 PC 모두 Co-op 바로 가기로 시작하세요.",
                           "主机的合作模组版本较旧（你：%s）。请在两台电脑上通过 Co-op 快捷方式启动游戏。", "主機的合作模組版本較舊（你：%s）。請在兩台電腦上透過 Co-op 捷徑啟動遊戲。"},
    /* (lobby-must round 2: shortened so that the one-line strip holds it at 5:4 too: tests/overlay_preview.c --banner-fit) */
    [LT_PARTNER_SILENT] = {"No answer from %s for %s s (in the background or loading)...", "Pas de réponse de %s depuis %s s (arrière-plan ou chargement)...",
                           "Nessuna risposta da %s da %s s (in background o in caricamento)...", "Keine Antwort von %s seit %s s (im Hintergrund oder lädt)...",
                           "Sin respuesta de %s desde hace %s s (en segundo plano o cargando)...", "Нет ответа от %s уже %s с (игра свернута или загружается)...",
                           "Brak odpowiedzi od %s od %s s (gra w tle lub wczytuje się)...", "Sem resposta de %s há %s s (em segundo plano ou carregando)...",
                           "%sから%s秒間応答がありません (バックグラウンドまたは読み込み中)...", "%s님의 응답이 %s초째 없습니다 (백그라운드 또는 로딩 중)...",
                           "%s已有%s秒无响应（游戏在后台或正在加载）...", "%s已有%s秒無回應（遊戲在背景或正在載入）..."},
    [LT_WAITING] = {"Waiting for a partner to join...", "En attente d'un partenaire...", "In attesa di un giocatore...", "Warte auf Beitritt eines Partners...",
                    "Esperando a un compañero...", "Ожидание партнера...", "Oczekiwanie na partnera...", "Aguardando um parceiro...",
                    "パートナーの参加を待っています...", "파트너의 참가를 기다리는 중...", "正在等待同伴加入...", "正在等待同伴加入..."},
    [LT_PARTNER_JOINED] = {"A partner has joined.", "Un partenaire a rejoint la partie.", "Un giocatore si è connesso.", "Ein Partner ist beigetreten.",
                           "Un compañero se ha unido.", "Партнер присоединился.", "Partner dołączył do gry.", "Um parceiro entrou.",
                           "パートナーが参加しました。", "파트너가 참가했습니다.", "同伴已加入。", "同伴已加入。"},
    [LT_ENTER_ADDR] = {"Enter the host's address (IP or IP:port):", "Saisissez l'adresse de l'hôte (IP ou IP:port) :", "Inserisci l'indirizzo dell'host (IP o IP:porta):",
                       "Adresse des Hosts eingeben (IP oder IP:Port):", "Introduce la dirección del anfitrión (IP o IP:puerto):", "Введите адрес хоста (IP или IP:порт):",
                       "Wpisz adres hosta (IP lub IP:port):", "Digite o endereço do anfitrião (IP ou IP:porta):", "ホストのアドレスを入力してください (IP または IP:ポート):",
                       "호스트 주소를 입력하세요 (IP 또는 IP:포트):", "请输入主机地址（IP 或 IP:端口）：", "請輸入主機位址（IP 或 IP:連接埠）："},
    [LT_CONNECTING] = {"Connecting...", "Connexion...", "Connessione...", "Verbinde...", "Conectando...", "Подключение...", "Łączenie...", "Conectando...",
                       "接続中...", "연결 중...", "正在连接...", "正在連線..."},
    [LT_NO_FRIENDS] = {"No friend is hosting a game right now.", "Aucun ami n'héberge de partie.", "Nessun amico sta ospitando una partita.",
                       "Zurzeit hostet kein Freund ein Spiel.", "Ningún amigo está creando una partida.", "Сейчас никто из друзей не создал игру.",
                       "Żaden znajomy nie prowadzi teraz gry.", "Nenhum amigo está hospedando agora.", "現在ホストしているフレンドはいません。",
                       "현재 호스트 중인 친구가 없습니다.", "目前没有好友在主持游戏。", "目前沒有好友在主持遊戲。"},
    [LT_REFRESH] = {"Refresh", "Actualiser", "Aggiorna", "Aktualisieren", "Actualizar", "Обновить", "Odśwież", "Atualizar", "更新", "새로 고침", "刷新", "重新整理"},
    [LT_NAME] = {"Name", "Nom", "Nome", "Name", "Nombre", "Имя", "Nazwa", "Nome", "名前", "이름", "名称", "名稱"},
    [LT_PING] = {"Ping", "Ping", "Ping", "Ping", "Ping", "Пинг", "Ping", "Ping", "ピング", "핑", "延迟", "延遲"},
    [LT_HINT] = {"Up/Down: choose   Left/Right: change   Enter: confirm   Esc: back", "Haut/Bas : choisir   Gauche/Droite : modifier   Entrée : valider   Échap : retour",
                 "Su/Giù: scegli   Sinistra/Destra: cambia   Invio: conferma   Esc: indietro", "Hoch/Runter: wählen   Links/Rechts: ändern   Eingabe: bestätigen   Esc: zurück",
                 "Arriba/Abajo: elegir   Izq./Der.: cambiar   Intro: confirmar   Esc: atrás", "Вверх/вниз: выбор   Влево/вправо: изменить   Ввод: подтвердить   Esc: назад",
                 "Góra/Dół: wybór   Lewo/Prawo: zmiana   Enter: zatwierdź   Esc: wstecz", "Cima/Baixo: escolher   Esq./Dir.: alterar   Enter: confirmar   Esc: voltar",
                 "上下: 選択   左右: 変更   Enter: 決定   Esc: 戻る", "위/아래: 선택   좌/우: 변경   Enter: 확인   Esc: 뒤로", "上/下：选择   左/右：更改   Enter：确认   Esc：返回",
                 "上/下：選擇   左/右：變更   Enter：確認   Esc：返回"},
    [LT_HINT_IP] = {"Type the address, Ctrl+V pastes, Enter connects, Esc goes back", "Saisissez l'adresse, Ctrl+V colle, Entrée connecte, Échap retour",
                    "Digita l'indirizzo, Ctrl+V incolla, Invio connette, Esc indietro", "Adresse eingeben, Strg+V fügt ein, Eingabe verbindet, Esc zurück",
                    "Escribe la dirección, Ctrl+V pega, Intro conecta, Esc atrás", "Введите адрес, Ctrl+V — вставить, Ввод — подключиться, Esc — назад",
                    "Wpisz adres, Ctrl+V wkleja, Enter łączy, Esc wstecz", "Digite o endereço, Ctrl+V cola, Enter conecta, Esc volta",
                    "アドレスを入力 / Ctrl+V で貼り付け / Enter で接続 / Esc で戻る", "주소를 입력하세요 / Ctrl+V 붙여넣기 / Enter 연결 / Esc 뒤로",
                    "输入地址，Ctrl+V 粘贴，Enter 连接，Esc 返回", "輸入位址，Ctrl+V 貼上，Enter 連線，Esc 返回"},
    [LT_YOUR_ADDR] = {"Your address (give it to your friend)", "Votre adresse (à donner à votre ami)", "Il tuo indirizzo (da dare al tuo amico)",
                      "Ihre Adresse (an Ihren Freund weitergeben)", "Tu dirección (dásela a tu amigo)", "Ваш адрес (сообщите его другу)",
                      "Twój adres (podaj go znajomemu)", "Seu endereço (passe ao seu amigo)", "あなたのアドレス (フレンドに伝えてください)",
                      "내 주소 (친구에게 알려 주세요)", "您的地址（告诉好友）", "您的位址（告訴好友）"},
    [LT_STEAM_HINT] = {"Friends join from Steam (Join Game) or from Co-Op > Join > Friend.", "Vos amis rejoignent via Steam (Rejoindre la partie) ou Coop > Rejoindre > Ami.",
                       "Gli amici entrano da Steam (Unisciti alla partita) o da Co-op > Unisciti > Amico.", "Freunde treten über Steam (Spiel beitreten) oder Koop > Beitreten > Freund bei.",
                       "Tus amigos se unen desde Steam (Unirse a la partida) o Cooperativo > Unirse > Amigo.",
                       "Друзья заходят через Steam (Присоединиться к игре) или Взаимодействие > Присоединиться > Друг.",
                       "Znajomi dołączają przez Steam (Dołącz do gry) lub Współpraca > Dołącz > Znajomy.", "Amigos entram pela Steam (Entrar na partida) ou Cooperação > Entrar > Amigo.",
                       "フレンドは Steam の「ゲームに参加」、またはメニューの協力プレイ > 参加 > フレンドから参加できます。", "친구는 Steam의 '게임 참가' 또는 협동플레이 > 참가 > 친구에서 들어올 수 있습니다.",
                       "好友可通过 Steam 的“加入游戏”，或在 协力游玩 > 加入 > 好友 中加入。", "好友可透過 Steam 的「加入遊戲」，或在 協力遊玩 > 加入 > 好友 中加入。"},
    [LT_STEAM_NA] = {"Steam is not available.", "Steam n'est pas disponible.", "Steam non è disponibile.", "Steam ist nicht verfügbar.", "Steam no está disponible.",
                     "Steam недоступен.", "Steam jest niedostępny.", "Steam não está disponível.", "Steam を利用できません。", "Steam을 사용할 수 없습니다.", "Steam 不可用。", "Steam 無法使用。"},
    [LT_NEED_READY] = {"The game can start when your partner is ready.", "La partie peut commencer quand votre partenaire est prêt.",
                       "La partita può iniziare quando il tuo compagno è pronto.", "Das Spiel kann starten, sobald Ihr Partner bereit ist.",
                       "La partida puede empezar cuando tu compañero esté listo.", "Игра начнется, когда партнер будет готов.",
                       "Gra ruszy, gdy partner będzie gotowy.", "O jogo pode começar quando seu parceiro estiver pronto.",
                       "パートナーの準備が完了すると開始できます。", "파트너가 준비를 마치면 시작할 수 있습니다.", "同伴准备就绪后即可开始。", "同伴準備就緒後即可開始。"},
    [LT_NOT_READY] = {"Not ready", "Pas prêt", "Non pronto", "Nicht bereit", "No listo", "Не готов", "Niegotowy", "Não pronto", "準備中", "준비 안 됨", "未准备", "未準備"},
    [LT_SEARCHING] = {"Searching...", "Recherche...", "Ricerca...", "Suche...", "Buscando...", "Поиск...", "Wyszukiwanie...", "Procurando...", "検索中...", "검색 중...", "正在搜索...", "正在搜尋..."},
    [LT_CHARACTER] = {"Character", "Personnage", "Personaggio", "Charakter", "Personaje", "Персонаж", "Postać", "Personagem", "キャラクター", "캐릭터", "角色", "角色"},
    [LT_STARTING] = {"Starting the game...", "Lancement de la partie...", "Avvio della partita...", "Spiel wird gestartet...", "Iniciando la partida...", "Запуск игры...",
                     "Uruchamianie gry...", "Iniciando o jogo...", "ゲームを開始しています...", "게임을 시작하는 중...", "正在开始游戏...", "正在開始遊戲..."},
    [LT_YOU] = {"(you)", "(vous)", "(tu)", "(Sie)", "(tú)", "(вы)", "(ty)", "(você)", "(あなた)", "(나)", "(你)", "(你)"},
    [LT_HOST_TAG] = {"Host", "Hôte", "Host", "Host", "Anfitrión", "Хост", "Host", "Anfitrião", "ホスト", "호스트", "主机", "主機"},
    [LT_PARTNER_READY] = {"Ready", "Prêt", "Pronto", "Bereit", "Listo", "Готов", "Gotowy", "Pronto", "準備完了", "준비 완료", "准备就绪", "準備就緒"},
    [LT_LEAVE_HINT] = {"Leaving ends the session for both players.", "Quitter met fin à la session pour les deux joueurs.", "Uscendo, la sessione termina per entrambi i giocatori.",
                       "Beim Verlassen endet die Sitzung für beide Spieler.", "Al salir, la sesión termina para ambos jugadores.", "При выходе сеанс завершится для обоих игроков.",
                       "Wyjście kończy sesję dla obu graczy.", "Sair encerra a sessão para os dois jogadores.", "退出すると、両方のプレイヤーのセッションが終了します。",
                       "나가면 두 플레이어 모두의 세션이 종료됩니다.", "离开后，双方玩家的游戏都将结束。", "離開後，雙方玩家的遊戲都將結束。"},
    [LT_SAVE_WARN] = {"Co-op play keeps its own save: your campaign save is not changed.",   /* 0.2.1 coopsave (was: co-op overwrites the campaign save) */
                      "La coop a sa propre sauvegarde : votre sauvegarde de campagne n'est pas modifiée.",
                      "La co-op ha un salvataggio a parte: il tuo salvataggio della campagna non viene modificato.",
                      "Koop hat einen eigenen Spielstand: Ihr Kampagnen-Spielstand bleibt unverändert.",
                      "El cooperativo tiene su propia partida guardada: tu partida de la campaña no cambia.",
                      "У кооператива своё сохранение: ваше сохранение кампании не изменяется.",
                      "Tryb współpracy ma własny zapis: Twój zapis kampanii pozostaje bez zmian.",
                      "O cooperativo tem um salvamento próprio: o salvamento da sua campanha não é alterado.",
                      "協力プレイは専用のセーブデータを使います。キャンペーンのセーブデータは変更されません。",
                      "협동 플레이는 별도의 저장 데이터를 사용합니다. 캠페인 저장 데이터는 바뀌지 않습니다.",
                      "协力游玩使用单独的存档：您的战役存档不会改变。", "協力遊玩使用單獨的存檔：您的戰役存檔不會改變。"},
    [LT_WAIT_LOAD] = {"Waiting for %s to finish loading...", "En attente de %s : chargement en cours...", "In attesa che %s finisca il caricamento...",
                      "Warte darauf, dass %s fertig geladen hat...", "Esperando a que %s termine de cargar...", "Ожидание: %s ещё загружает уровень...",
                      "Czekam, aż %s skończy wczytywanie...", "Aguardando %s terminar de carregar...", "%sの読み込み完了を待っています...",
                      "%s님의 로딩이 끝나기를 기다리는 중...", "正在等待%s完成加载...", "正在等待%s完成載入..."},
    [LT_LEG_OK] = {"Confirm", "Valider", "Conferma", "Bestätigen", "Confirmar", "Подтвердить", "Zatwierdź", "Confirmar", "決定", "확인", "确认", "確認"},
    /* downed / revive (stage 9; the wording is ours and has not been read by native speakers) */
    [LT_DOWNED] = {"Downed / revive", "Agonie / réanimation", "Agonia / rianimazione", "Agonie / Wiederbelebung", "Agonía / reanimación", "Агония / поднять напарника", "Agonia / podnoszenie",
                   "Agonia / reviver", "瀕死・蘇生", "빈사 / 소생", "濒死 / 救助", "瀕死 / 救助"},
    [LT_DOWN_SELF] = {"You are down - %s s until you bleed out. Your partner can raise you.", "Vous êtes à terre - %s s avant la mort. Votre partenaire peut vous relever.",
                      "Sei a terra - %s s prima di morire. Il tuo compagno può rialzarti.", "Sie liegen am Boden - noch %s s bis zum Verbluten. Ihr Partner kann Sie aufrichten.",
                      "Estás en el suelo - %s s hasta morir desangrado. Tu compañero puede levantarte.", "Вы упали - до смерти %s с. Партнер может вас поднять.",
                      "Leżysz - %s s do wykrwawienia. Partner może cię podnieść.", "Você caiu - %s s até sangrar. Seu parceiro pode levantá-lo.",
                      "倒れています - 出血死まであと%s秒。パートナーが助け起こせます。", "쓰러졌습니다 - 과다 출혈까지 %s초. 파트너가 일으켜 줄 수 있습니다.",
                      "你已倒下 - 还有%s秒就会失血而亡。同伴可以救你。", "你已倒下 - 還有%s秒就會失血而亡。同伴可以救你。"},
    [LT_DOWN_PARTNER] = {"Your partner is down - hold the action button next to him (%s s left).", "Votre partenaire est à terre - maintenez le bouton d'action près de lui (%s s).",
                         "Il tuo compagno è a terra - tieni premuto il tasto azione vicino a lui (%s s).", "Ihr Partner liegt am Boden - Aktionstaste neben ihm halten (%s s).",
                         "Tu compañero está en el suelo - mantén el botón de acción a su lado (%s s).", "Партнер упал - удерживайте кнопку действия рядом с ним (%s с).",
                         "Partner leży - przytrzymaj przycisk akcji obok niego (%s s).", "Seu parceiro caiu - segure o botão de ação ao lado dele (%s s).",
                         "パートナーが倒れています - そばでアクションボタンを長押ししてください (%s秒)。", "파트너가 쓰러졌습니다 - 곁에서 행동 버튼을 길게 누르세요 (%s초).",
                         "同伴倒下了 - 在他身旁按住动作键 (%s秒)。", "同伴倒下了 - 在他身旁按住動作鍵 (%s秒)。"},

    /* ---- 0.2.0 lobby (lobby-must; our wording, not read by native speakers yet) ---- */
    /* stock strings: the fallback (a table that is not loaded) in every language, the game's own words; the game shows its own. The K_FORMAT ones: the game's
       <USER> / <SWAP> = %s here */
    [LT_REMOVED] = {"You have been removed from the session by the host.", "Vous avez été expulsé de la partie par l'hôte.", "L'host ti ha rimosso dalla sessione.", "Sie wurden vom Host aus der Sitzung entfernt.", "Has sido expulsado de la sesión.", "Создатель игры удалил вас из сеанса.", "Gospodarz usunął cię z sesji.", "Você foi removido da sessão pelo host.", "ホストからの指示で セッションから退出しました。", "호스트에 의해 세션에서 퇴장 처리되었습니다.", "游戏的主办者已将你移出游戏。", "遊戲的主辦者已將你移出遊戲。"},
    [LT_REF_BUILD] = {"You have a different version of the game and could not join this session.", "Vous ne pouvez pas joindre cette session parce que vous possédez une version différente du jeu.", "Possiedi una versione diversa del gioco. Impossibile partecipare a questa sessione.", "Sie haben eine andere Version des Spiels und können der Sitzung deshalb nicht beitreten.", "No puedes unirte a esta sesión de juego. La versión del juego que tienes es diferente.", "Не удалось присоединиться к сеансу: у вас другая версия игры.", "Nie udało się dołączyć do sesji, ponieważ posiadasz inną wersję gry.", "Você tem uma versão de jogo diferente e não foi possível entrar nesta sessão.", "相手とのゲームバージョンが異なるため セッションに参加できません。", "다른 플레이어와 게임의 버전이 달라서 이 세션에 참가할 수 없습니다.", "你的游戏版本不符， 无法加入此游戏。", "你的遊戲版本不符， 無法加入此遊戲。"},
    [LT_KICK_ASK] = {"Do you want to kick %s?", "Expulser %s de la partie ?", "Vuoi espellere %s?", "Möchten Sie %s entfernen?", "¿Quieres expulsar a %s?", "Удалить игрока %s?", "Czy chcesz usunąć: %s?", "Deseja expulsar %s?", "%sを追放しますか？", "%s 님을 강제 퇴장시키시겠습니까?", "确定要移除%s吗？", "確定要移除%s嗎？"},
    [LT_PARTNER_LEFT_ROOM] = {"%s has left the session.", "%s a quitté le lobby.", "%s ha lasciato la sessione.", "%s hat die Sitzung verlassen.", "%s ha dejado la sesión.", "Игрок %s вышел из сеанса.", "%s opuszcza sesję.", "%s saiu da sessão.", "%sが退室しました", "%s 님께서 세션 퇴장.", "%s离开了进行中的游戏。", "%s離開了進行中的遊戲。"},
    [LT_PARTNER_LOST_ROOM] = {"%s has disconnected.", "%s s'est déconnecté.", "%s si è disconnesso.", "Verbindung zu %s wurde unterbrochen.", "%s se ha desconectado.", "Игрок %s отключился.", "%s rozłącza się.", "%s desconectou.", "%sが切断されました", "%s 님의 접속이 끊김.", "%s已经脱机。", "%s已經離線。"},
    [LT_SESSION_OVER] = {"Session has been concluded.", "La session est terminée.", "La sessione è stata terminata.", "Sitzung wurde beendet.", "La sesión ha concluido.", "Сеанс завершен.", "Sesja zakończona.", "A sessão foi concluída.", "セッションが終了しました。", "세션이 종결되었습니다.", "游戏已结束。", "遊戲已結束。"},
    /* refusals: shown after the stock "Failed to join session." (at most 159 bytes: tests/test_resil.c) */
    [LT_REF_FULL] = {"This session is full.", "Cette session est complète.", "Questa sessione è al completo.", "Diese Sitzung ist voll.", "Esta sesión está completa.",
                     "В этом сеансе нет свободных мест.", "Ta sesja jest pełna.", "Esta sessão está cheia.", "このセッションは満員です。", "이 세션은 인원이 가득 찼습니다.",
                     "此游戏的人数已满。", "此遊戲的人數已滿。"},
    [LT_REF_STARTED] = {"This co-op game has already started. Joining a running game is not possible in this version.",
                        "Cette partie coop a déjà commencé. Rejoindre une partie en cours est impossible dans cette version.",
                        "Questa partita co-op è già iniziata. In questa versione non ci si può unire a una partita in corso.",
                        "Dieses Koop-Spiel läuft bereits. In dieser Version kann man einem laufenden Spiel nicht beitreten.",
                        "Esta partida cooperativa ya empezó. En esta versión no se puede unir a una partida en curso.",
                        "Эта кооп-игра уже началась. В этой версии нельзя войти в идущую игру.",
                        "Ta gra w trybie współpracy już trwa. W tej wersji nie można dołączyć do trwającej gry.",
                        "Este jogo cooperativo já começou. Nesta versão não é possível entrar em um jogo em andamento.",
                        "この協力プレイは始まっています。このバージョンでは進行中のゲームに参加できません。",
                        "이 협동 플레이는 이미 시작되었습니다. 이 버전에서는 진행 중인 게임에 참가할 수 없습니다.",
                        "此协力游戏已经开始。此版本无法加入进行中的游戏。", "此協力遊戲已經開始。此版本無法加入進行中的遊戲。"},
    [LT_REF_NOANSWER] = {"No answer from the host. Check the address and that the host is in the Co-Op room.",
                         "Pas de réponse de l'hôte. Vérifiez l'adresse et que l'hôte est dans le salon coop.",
                         "Nessuna risposta dall'host. Controlla l'indirizzo e che l'host sia nella stanza co-op.",
                         "Keine Antwort vom Host. Prüfen Sie die Adresse und ob der Host im Koop-Raum ist.",
                         "El anfitrión no responde. Revisa la dirección y que esté en la sala cooperativa.",
                         "Хост не отвечает. Проверьте адрес и что хост в комнате кооп-игры.",
                         "Brak odpowiedzi hosta. Sprawdź adres i czy host jest w pokoju gry.",
                         "O anfitrião não responde. Verifique o endereço e se ele está na sala cooperativa.",
                         "ホストから応答がありません。アドレスと、ホストが協力プレイの部屋にいるか確認してください。",
                         "호스트가 응답하지 않습니다. 주소와 호스트가 협동 대기실에 있는지 확인하세요.",
                         "主机没有回应。请检查地址，并确认主机在协力房间中。", "主機沒有回應。請檢查位址，並確認主機在協力房間中。"},
    [LT_REF_BAD] = {"The host did not accept the connection.", "L'hôte n'a pas accepté la connexion.", "L'host non ha accettato la connessione.",
                    "Der Host hat die Verbindung nicht angenommen.", "El anfitrión no aceptó la conexión.", "Хост не принял подключение.", "Host nie przyjął połączenia.",
                    "O anfitrião não aceitou a conexão.", "ホストが接続を受け付けませんでした。", "호스트가 연결을 수락하지 않았습니다.", "主机未接受连接。", "主機未接受連線。"},
    /* in-game strips (one line across the top: tests check the width with the strip's own font) */
    [LT_PARTNER_LEFT] = {"%s has left the game. The game continues with the AI partner.", "%s a quitté la partie. Elle continue avec le partenaire IA.",
                         "%s ha lasciato la partita. Si continua con il compagno IA.", "%s hat das Spiel verlassen. Es geht mit dem KI-Partner weiter.",
                         "%s ha salido de la partida. Sigue con el compañero de la IA.", "Игрок %s вышел из игры. Дальше играет ИИ-партнер.",
                         "%s opuszcza grę. Gra toczy się dalej z partnerem SI.", "%s saiu do jogo. O jogo continua com o parceiro da IA.",
                         "%sがゲームから退出しました。AIパートナーとゲームを続けます。", "%s님이 게임에서 나갔습니다. AI 파트너와 게임을 계속합니다.",
                         "%s已离开游戏。游戏将由AI同伴继续。", "%s已離開遊戲。遊戲將由AI同伴繼續。"},
    [LT_PARTNER_LOST] = {"Connection to %s was lost. The game continues with the AI partner.", "Connexion avec %s perdue. La partie continue avec le partenaire IA.",
                         "Connessione con %s persa. Si continua con il compagno IA.", "Verbindung zu %s verloren. Es geht mit dem KI-Partner weiter.",
                         "Se perdió la conexión con %s. Sigue con el compañero de la IA.", "Связь с игроком %s потеряна. Дальше играет ИИ-партнер.",
                         "Utracono połączenie z %s. Gra toczy się dalej z partnerem SI.", "A conexão com %s caiu. O jogo continua com o parceiro da IA.",
                         "%sとの接続が切れました。AIパートナーとゲームを続けます。", "%s님과의 연결이 끊어졌습니다. AI 파트너와 게임을 계속합니다.",
                         "与%s的连接已中断。游戏将由AI同伴继续。", "與%s的連線已中斷。遊戲將由AI同伴繼續。"},
    [LT_HOST_ENDED] = {"The host has ended the session. Returning to the title screen...", "L'hôte a mis fin à la session. Retour à l'écran titre...",
                       "L'host ha terminato la sessione. Ritorno alla schermata del titolo...", "Der Host hat die Sitzung beendet. Zurück zum Titelbildschirm...",
                       "El anfitrión ha terminado la sesión. Volviendo a la pantalla de título...", "Хост завершил сеанс. Возврат к главному экрану...",
                       "Host zakończył sesję. Powrót do ekranu tytułowego...", "O anfitrião encerrou a sessão. Voltando para a tela de título...",
                       "ホストがセッションを終了しました。タイトル画面に戻ります...", "호스트가 세션을 종료했습니다. 타이틀 화면으로 돌아갑니다...",
                       "主机已结束游戏。正在返回标题画面...", "主機已結束遊戲。正在返回標題畫面..."},
    [LT_HOST_LOST] = {"Connection to the host was lost. Returning to the title screen...", "Connexion avec l'hôte perdue. Retour à l'écran titre...",
                      "Connessione con l'host persa. Ritorno alla schermata del titolo...", "Verbindung zum Host verloren. Zurück zum Titelbildschirm...",
                      "Se perdió la conexión con el anfitrión. Volviendo a la pantalla de título...", "Связь с хостом потеряна. Возврат к главному экрану...",
                      "Utracono połączenie z hostem. Powrót do ekranu tytułowego...", "A conexão com o anfitrião caiu. Voltando para a tela de título...",
                      "ホストとの接続が切れました。タイトル画面に戻ります...", "호스트와의 연결이 끊어졌습니다. 타이틀 화면으로 돌아갑니다...",
                      "与主机的连接已中断。正在返回标题画面...", "與主機的連線已中斷。正在返回標題畫面..."},
    [LT_EP1_END] = {"Episode 1 is complete. Co-op play ends here in this version: the session is closed.",
                    "L'épisode 1 est terminé. La coop s'arrête ici dans cette version : session fermée.",
                    "Episodio 1 completato. In questa versione la co-op finisce qui: sessione chiusa.",
                    "Episode 1 ist geschafft. In dieser Version endet Koop hier: Sitzung beendet.",
                    "Episodio 1 completado. En esta versión el cooperativo acaba aquí: sesión cerrada.",
                    "Эпизод 1 пройден. В этой версии кооператив здесь заканчивается: сеанс закрыт.",
                    "Epizod 1 ukończony. W tej wersji współpraca kończy się tutaj: sesja zamknięta.",
                    "Episódio 1 concluído. Nesta versão o cooperativo termina aqui: sessão encerrada.",
                    "エピソード1クリア。このバージョンの協力プレイはここまでです (セッション終了)。",
                    "에피소드 1 완료. 이 버전의 협동 플레이는 여기까지입니다 (세션 종료).",
                    "第1章已完成。此版本的协力游玩到此为止（游戏已关闭）。", "第1章已完成。此版本的協力遊玩到此為止（遊戲已關閉）。"},
    /* waiting lines of the lobby */
    [LT_CONNECTING_TO] = {"Connecting to %s...", "Connexion à %s...", "Connessione a %s...", "Verbinde mit %s...", "Conectando con %s...", "Подключение к %s...",
                          "Łączenie z %s...", "Conectando a %s...", "%sに接続中...", "%s에 연결 중...", "正在连接%s...", "正在連線至%s..."},
    [LT_AWAITING] = {"Awaiting reply from %s...", "En attente de la réponse de %s...", "In attesa di risposta da %s...", "Warte auf Antwort von %s...",
                     "Esperando respuesta de %s...", "Ожидание ответа от %s...", "Oczekiwanie na odpowiedź od %s...", "Aguardando resposta de %s...",
                     "%sの応答を待っています...", "%s의 응답을 기다리는 중...", "正在等待%s的回应...", "正在等待%s的回應..."},
    [LT_HOST_WAIT_READY] = {"Waiting for %s to get ready...", "En attente que %s soit prêt...", "In attesa che %s sia pronto...", "Warte, bis %s bereit ist...",
                            "Esperando a que %s esté listo...", "Ожидание готовности игрока %s...", "Czekam, aż %s będzie gotowy...", "Aguardando %s ficar pronto...",
                            "%sの準備完了を待っています...", "%s님의 준비를 기다리는 중...", "正在等待%s准备就绪...", "正在等待%s準備就緒..."},
    [LT_HOST_CAN_START] = {"%s is ready. You can start the game.", "%s est prêt. Vous pouvez lancer la partie.", "%s è pronto. Puoi avviare la partita.",
                           "%s ist bereit. Sie können das Spiel starten.", "%s está listo. Ya puedes empezar la partida.", "Игрок %s готов. Можно начинать игру.",
                           "%s jest gotowy. Możesz rozpocząć grę.", "%s está pronto. Você já pode iniciar o jogo.", "%sの準備が完了しました。ゲームを開始できます。",
                           "%s님이 준비되었습니다. 게임을 시작할 수 있습니다.", "%s已准备就绪。可以开始游戏。", "%s已準備就緒。可以開始遊戲。"},
    [LT_GUEST_PICK] = {"Choose your character, then mark yourself ready.", "Choisissez votre personnage, puis indiquez que vous êtes prêt.",
                       "Scegli il tuo personaggio, poi segnalati come pronto.", "Wählen Sie Ihren Charakter und melden Sie sich dann bereit.",
                       "Elige tu personaje y luego indica que estás listo.", "Выберите персонажа, затем отметьте готовность.", "Wybierz postać, a następnie zgłoś gotowość.",
                       "Escolha seu personagem e depois confirme que está pronto.", "キャラクターを選んでから、準備完了にしてください。",
                       "캐릭터를 선택한 다음 준비 완료를 선택하세요.", "请选择角色，然后选择准备就绪。", "請選擇角色，然後選擇準備就緒。"},
    [LT_GUEST_WAIT_HOST] = {"Waiting for %s to start the game...", "En attente du lancement de la partie par %s...", "In attesa che %s avvii la partita...",
                            "Warte darauf, dass %s das Spiel startet...", "Esperando a que %s empiece la partida...", "Ждем, когда %s начнет игру...",
                            "Czekam, aż %s rozpocznie grę...", "Aguardando %s iniciar o jogo...", "%sがゲームを開始するのを待っています...",
                            "%s님이 게임을 시작하기를 기다리는 중...", "正在等待%s开始游戏...", "正在等待%s開始遊戲..."},
    [LT_KICK] = {"Remove partner", "Expulser le partenaire", "Espelli il compagno", "Partner entfernen", "Expulsar al compañero", "Удалить партнера", "Usuń partnera",
                 "Expulsar parceiro", "パートナーを追放", "파트너 강제 퇴장", "移除同伴", "移除同伴"},
    [LT_EP1_ONLY] = {"Co-op play covers Episode 1 in this version (both halves).", "Dans cette version, la coop couvre l'épisode 1 (les deux parties).",
                     "In questa versione la co-op comprende l'episodio 1 (entrambe le parti).", "In dieser Version umfasst Koop Episode 1 (beide Hälften).",
                     "En esta versión, el cooperativo abarca el episodio 1 (las dos partes).", "В этой версии кооператив охватывает эпизод 1 (обе половины).",
                     "W tej wersji tryb współpracy obejmuje epizod 1 (obie części).", "Nesta versão, o cooperativo cobre o episódio 1 (as duas partes).",
                     "このバージョンの協力プレイはエピソード1 (前半・後半) に対応しています。", "이 버전의 협동 플레이는 에피소드 1(전반부와 후반부)을 지원합니다.",
                     "此版本的协力游玩支持第1章（前后两部分）。", "此版本的協力遊玩支援第1章（前後兩部分）。"},
    [LT_HOST_NET_HINT] = {"Over the internet your friend needs your public address, and UDP port %s must be forwarded to this PC (allow the game in Windows Firewall).",
                          "Par Internet, votre ami a besoin de votre adresse publique et le port UDP %s doit être redirigé vers ce PC (autorisez le jeu dans le pare-feu Windows).",
                          "Via Internet il tuo amico ha bisogno del tuo indirizzo pubblico e la porta UDP %s va inoltrata a questo PC (consenti il gioco in Windows Firewall).",
                          "Über das Internet braucht Ihr Freund Ihre öffentliche Adresse, und UDP-Port %s muss an diesen PC weitergeleitet werden (Spiel in der Windows-Firewall zulassen).",
                          "Por Internet, tu amigo necesita tu dirección pública y el puerto UDP %s debe redirigirse a este PC (permite el juego en el Firewall de Windows).",
                          "Через интернет другу нужен ваш внешний адрес, а UDP-порт %s должен быть проброшен на этот ПК (разрешите игру в брандмауэре Windows).",
                          "Przez internet znajomy potrzebuje Twojego adresu publicznego, a port UDP %s musi być przekierowany na ten komputer (zezwól na grę w Zaporze Windows).",
                          "Pela internet, seu amigo precisa do seu endereço público e a porta UDP %s deve ser redirecionada para este PC (permita o jogo no Firewall do Windows).",
                          "インターネット経由では、フレンドにグローバルアドレスを伝え、UDPポート%sをこのPCに転送してください (Windowsファイアウォールでゲームを許可)。",
                          "인터넷으로 플레이하려면 친구에게 공인 주소를 알려 주고 UDP 포트 %s를 이 PC로 포워딩하세요 (Windows 방화벽에서 게임 허용).",
                          "通过互联网连接时，好友需要你的公网地址，并需将UDP端口%s转发到此电脑（在Windows防火墙中允许游戏）。",
                          "透過網際網路連線時，好友需要你的公用位址，並需將UDP連接埠%s轉送到此電腦（在Windows防火牆中允許遊戲）。"},
    /* the reason of a Steam join that failed, after the stock "Failed to join session." (lang 2026-11-08: before, the backend's English text was shown in every
       language; lobby_backend_reason picks one; at most 159 bytes: tests/test_resil.c) */
    [LT_REF_GONE] = {"This game is not open any more.", "Cette partie n'est plus ouverte.", "Questa partita non è più aperta.", "Dieses Spiel ist nicht mehr offen.",
                     "Esta partida ya no está abierta.", "Эта игра больше не открыта.", "Ta gra nie jest już otwarta.", "Este jogo não está mais aberto.",
                     "このゲームはもう開かれていません。", "이 게임은 더 이상 열려 있지 않습니다.", "此游戏已不再开放。", "此遊戲已不再開放。"},
    [LT_REF_DENIED] = {"Steam did not allow joining this game (it may be for friends only).", "Steam n'a pas autorisé à rejoindre cette partie (elle est peut-être réservée aux amis).",
                       "Steam non ha permesso di unirsi a questa partita (forse è solo per amici).", "Steam hat den Beitritt nicht erlaubt (das Spiel ist vielleicht nur für Freunde).",
                       "Steam no permitió unirse a esta partida (puede que sea solo para amigos).", "Steam не разрешил войти в эту игру (возможно, она только для друзей).",
                       "Steam nie pozwolił dołączyć do tej gry (może być tylko dla znajomych).", "A Steam não permitiu entrar neste jogo (talvez seja só para amigos).",
                       "Steam がこのゲームへの参加を許可しませんでした (フレンド限定の可能性があります)。", "Steam에서 이 게임 참가를 허용하지 않았습니다 (친구 전용일 수 있습니다).",
                       "Steam 不允许加入此游戏（可能仅限好友）。", "Steam 不允許加入此遊戲（可能僅限好友）。"},
    [LT_REF_NOLOBBY] = {"No co-op game of this friend was found: the host must be in the Co-Op room.", "Aucune partie coop de cet ami : l'hôte doit être dans le salon coop.",
                        "Nessuna partita co-op di questo amico: l'host deve essere nella stanza co-op.", "Kein Koop-Spiel dieses Freundes gefunden: Der Host muss im Koop-Raum sein.",
                        "No hay partida cooperativa de este amigo: el anfitrión debe estar en la sala cooperativa.", "Кооп-игра этого друга не найдена: хост должен быть в комнате кооп-игры.",
                        "Nie znaleziono gry tego znajomego: host musi być w pokoju gry.", "Nenhum jogo cooperativo desse amigo: o anfitrião deve estar na sala cooperativa.",
                        "このフレンドの協力プレイが見つかりません。ホストが協力プレイの部屋にいる必要があります。", "이 친구의 협동 게임을 찾지 못했습니다. 호스트가 협동 대기실에 있어야 합니다.",
                        "未找到该好友的协力游戏：主机必须在协力房间中。", "未找到該好友的協力遊戲：主機必須在協力房間中。"},
    [LT_REF_NOTCOOP] = {"This Steam game is not a co-op game of this mod.", "Cette partie Steam n'est pas une partie de ce mod coop.", "Questa partita Steam non è una partita di questa mod co-op.",
                        "Dieses Steam-Spiel ist kein Spiel dieser Koop-Mod.", "Esta partida de Steam no es de este mod cooperativo.", "Эта игра в Steam не относится к этому кооп-моду.",
                        "Ta gra Steam nie jest grą tego moda co-op.", "Este jogo da Steam não é deste mod cooperativo.", "この Steam のゲームはこの協力 MOD のゲームではありません。",
                        "이 Steam 게임은 이 협동 모드의 게임이 아닙니다.", "此 Steam 游戏不是本合作模组的游戏。", "此 Steam 遊戲不是本合作模組的遊戲。"},
    [LT_REF_STEAM_NOANSWER] = {"The host did not answer over Steam (a firewall or connection problem?).", "L'hôte n'a pas répondu via Steam (pare-feu ou problème de connexion ?).",
                               "L'host non ha risposto tramite Steam (firewall o problema di connessione?).", "Der Host hat über Steam nicht geantwortet (Firewall oder Verbindungsproblem?).",
                               "El anfitrión no respondió por Steam (¿cortafuegos o problema de conexión?).", "Хост не ответил через Steam (брандмауэр или проблема со связью?).",
                               "Host nie odpowiedział przez Steam (zapora lub problem z połączeniem?).", "O anfitrião não respondeu pela Steam (firewall ou problema de conexão?).",
                               "ホストから Steam 経由の応答がありません (ファイアウォールや接続の問題?)。", "호스트가 Steam으로 응답하지 않았습니다 (방화벽 또는 연결 문제?).",
                               "主机没有通过 Steam 回应（防火墙或连接问题？）。", "主機沒有透過 Steam 回應（防火牆或連線問題？）。"},
    /* the ping in the room table (lang, live 2026-11-09: "5 ms" in the Russian and Chinese rooms); "ms" is the unit symbol of the Latin-script languages */
    [LT_MS] = {"%s ms", "%s ms", "%s ms", "%s ms", "%s ms", "%s мс", "%s ms", "%s ms", "%sミリ秒", "%s밀리초", "%s毫秒", "%s毫秒"},
    /* 0.2.1 coopsave (our wording; not read by native speakers) */
    [LT_CONTINUE] = {"Continue", "Continuer", "Continua", "Fortsetzen", "Continuar", "Продолжить", "Kontynuuj", "Continuar", "コンティニュー", "이어하기", "继续游戏", "繼續遊戲"},
    [LT_CONT_NEED] = {"Continue needs a co-op save of this half on both PCs. Choose No to start a new game.",
                      "Pour continuer, les deux PC doivent avoir une sauvegarde coop de cette partie. Choisissez Non pour une nouvelle partie.",
                      "Per continuare, entrambi i PC devono avere un salvataggio co-op di questa parte. Scegli No per una nuova partita.",
                      "Zum Fortsetzen brauchen beide PCs einen Koop-Spielstand dieser Hälfte. Wählen Sie Nein für ein neues Spiel.",
                      "Para continuar, ambos PC necesitan una partida cooperativa de esta parte. Elige No para empezar una nueva partida.",
                      "Для продолжения на обоих ПК нужно кооперативное сохранение этой половины. Выберите «Нет», чтобы начать новую игру.",
                      "Aby kontynuować, oba komputery muszą mieć zapis trybu współpracy tej części. Wybierz Nie, aby zacząć nową grę.",
                      "Para continuar, os dois PCs precisam de um salvamento cooperativo desta parte. Escolha Não para começar um novo jogo.",
                      "コンティニューするには、両方のPCにこのパートの協力プレイのセーブデータが必要です。新しく始めるには「いいえ」を選んでください。",
                      "이어하려면 두 PC 모두에 이 파트의 협동 플레이 저장 데이터가 있어야 합니다. 새로 시작하려면 '아니요'를 선택하세요.",
                      "继续游戏需要两台电脑都有这一部分的协力游玩存档。选择“否”开始新游戏。",
                      "繼續遊戲需要兩台電腦都有這一部分的協力遊玩存檔。選擇「否」開始新遊戲。"},
    /* the host's pause menu row (drawn by the game's own GUI with its bitmap font: the CJK words use characters the game's own texts of that language contain,
       lab\lm_glyphs.py) */
    [LT_END_COOP] = {"End Co-Op", "Arrêter la coop", "Termina co-op", "Koop beenden", "Terminar cooperativo", "Завершить кооператив", "Zakończ współpracę",
                     "Encerrar cooperativo", "協力プレイ終了", "협동 플레이 종료", "结束协力游玩", "結束協力遊玩"},
    [LT_END_COOP_HELP] = {"Your partner leaves the session; you keep playing with the AI partner.", "Votre partenaire quitte la session ; vous continuez avec le partenaire IA.",
                          "Il compagno lascia la sessione; continui con il compagno IA.", "Ihr Partner verlässt die Sitzung; Sie spielen mit dem KI-Partner weiter.",
                          "Tu compañero sale de la sesión; sigues con el compañero de la IA.", "Партнер покинет сеанс; вы продолжите с партнером под управлением ИИ.",
                          "Partner opuści sesję; grasz dalej z partnerem SI.", "Seu parceiro sai da sessão; você continua com o parceiro da IA.",
                          "パートナーがセッションから退出し、AIパートナーとプレイを続けます。", "파트너가 세션에서 나가고 AI 파트너와 계속 플레이합니다.",
                          "同伴将离开游戏，你将与AI同伴继续游戏。", "同伴將離開遊戲，你將與AI同伴繼續遊戲。"},
};

const char *lobby_own_text(int text, int lang)
{
    if (text < 0 || text >= LT_COUNT) return "";
    if (lang < 0 || lang > 11) lang = 0;
    const char *s = T[text][lang];
    if (!s) s = T[text][0];
    return s ? s : "";
}

/* copy at most size - 1 bytes, never ending inside a UTF-8 sequence */
static const char *utf8_fit(char *out, size_t size, const char *s)
{
    if (!out || !size) return out;
    size_t n = strlen(s);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* s[n] would be the first byte left out: back up to the start of its character */
    }
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* a %s-only format of our table with up to two values (the table has no other % sequence: tests/test_resil.c) */
static const char *fmt_fit(char *out, size_t size, const char *fmt, const char *a, const char *b)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, fmt, a ? a : "", b ? b : "");
    return utf8_fit(out, size, tmp);
}

const char *lobby_refusal_text(int code, const char *host_mod, const char *own_mod, int lang, char *out, size_t size)
{
    if (!host_mod) host_mod = "";
    if (!own_mod) own_mod = "";
    switch (code) {
    case CP_REF_VERSION: return utf8_fit(out, size, lobby_own_text(LT_REF_PROTO, lang));
    case CP_REF_MODVER:
        if (host_mod[0]) return fmt_fit(out, size, lobby_own_text(LT_REF_MODVER, lang), host_mod, own_mod);
        return fmt_fit(out, size, lobby_own_text(LT_REF_MODVER_OLD, lang), own_mod, NULL);
    case CP_REF_FULL: return utf8_fit(out, size, lobby_own_text(LT_REF_FULL, lang));
    case CP_REF_STARTED: return utf8_fit(out, size, lobby_own_text(LT_REF_STARTED, lang));
    case CP_REF_BAD: return utf8_fit(out, size, lobby_own_text(LT_REF_BAD, lang));
    case CS_REF_NOANSWER: return utf8_fit(out, size, lobby_own_text(LT_REF_NOANSWER, lang));
    case CP_REF_BUILD: return utf8_fit(out, size, lobby_own_text(LT_REF_BUILD, lang));     /* stock (menu.c shows the game's words); the English fallback here */
    case CP_REF_REMOVED: return utf8_fit(out, size, lobby_own_text(LT_REMOVED, lang));     /* stock, as BUILD */
    default: return NULL;   /* 0: a backend's own reason (Steam: no such lobby, not ours, full): the session's text stays */
    }
}

int lobby_backend_reason(const char *why)
{
    /* the English reasons of coop_steam.c (client_fail / client_joining: "Steam: ..."), matched by their fixed words; tests/test_lang.c feeds every reason that file
       writes. Anything else: -1 (the caller shows no reason rather than English) */
    static const struct { const char *words; int text; } map[] = {
        {"it does not exist any more", LT_REF_GONE},          /* JoinLobby response 2 */
        {": full", LT_REF_FULL},                               /* response 4 */
        {"not allowed", LT_REF_DENIED},                        /* response 3 (friends only) */
        {"would not let us into the lobby", LT_REF_DENIED},    /* any other response */
        {"refused JoinLobby", LT_REF_DENIED},
        {"no co-op lobby of that player was found", LT_REF_NOLOBBY},
        {"is not a Resident Evil Revelations 2 co-op lobby", LT_REF_NOTCOOP},
        {"the lobby has no other owner", LT_REF_NOTCOOP},
        {"the host left the lobby before the link opened", LT_REF_GONE},
        {"did not answer on the Steam pipe", LT_REF_STEAM_NOANSWER},
        {"timed out", LT_REF_STEAM_NOANSWER},
    };
    if (!why || !why[0]) return -1;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strstr(why, map[i].words)) return map[i].text;
    return -1;
}

int lobby_text_args(int text)
{
    switch (text) {
    case LT_REF_MODVER: case LT_PARTNER_SILENT: return 2;
    case LT_REF_MODVER_OLD: case LT_WAIT_LOAD: case LT_DOWN_SELF: case LT_DOWN_PARTNER: case LT_KICK_ASK: case LT_PARTNER_LEFT_ROOM: case LT_PARTNER_LOST_ROOM:
    case LT_PARTNER_LEFT: case LT_PARTNER_LOST: case LT_CONNECTING_TO: case LT_AWAITING: case LT_HOST_WAIT_READY: case LT_HOST_CAN_START: case LT_GUEST_WAIT_HOST:
    case LT_HOST_NET_HINT: case LT_MS:
        return 1;
    default: return 0;
    }
}

const char *lobby_format(const char *fmt, const char *a, const char *b, char *out, size_t size)
{
    /* every % that is not "%s" is printed as itself (a stock string could carry one); at most two values */
    char safe[600];
    size_t o = 0;
    int args = 0;
    for (const char *p = fmt ? fmt : ""; *p && o + 3 < sizeof safe; p++) {
        if (*p == '%' && p[1] == 's' && args < 2) safe[o++] = '%', safe[o++] = 's', p++, args++;
        else if (*p == '%') safe[o++] = '%', safe[o++] = '%';
        else safe[o++] = *p;
    }
    safe[o] = 0;
    return fmt_fit(out, size, safe, a, b);
}

int lobby_loss_text(int what, int a, int b, int is_host)
{
    if (is_host && what == CS_N_PEER_LEAVE) {
        if (b == CS_GONE_EP_END) return LT_EP1_END;
        if (b == CS_GONE_REPLACED) return -1;   /* (the lab's rejoin: the same player is back on a new link) */
        return a == 1 || b == CS_GONE_SILENT ? LT_PARTNER_LOST : LT_PARTNER_LEFT;
    }
    if (!is_host && what == CS_N_LOST) {
        if (a == 1 && b == CP_LEAVE_EP_END) return LT_EP1_END;
        return a == 1 ? LT_HOST_ENDED : LT_HOST_LOST;
    }
    return -1;
}

const char *lobby_display_name(const char *wire_name, const char *player_word, char *out, size_t size)
{
    if (!wire_name) wire_name = "";
    if ((!strcmp(wire_name, "Player 1") || !strcmp(wire_name, "Player 2")) && player_word && player_word[0]) {
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s %c", player_word, wire_name[7]);
        return utf8_fit(out, size, tmp);
    }
    return utf8_fit(out, size, wire_name[0] ? wire_name : "?");
}

const char *lobby_loss_line(int text, const char *name, const char *player_word, int lang, char *out, size_t size)
{
    char nm[64];
    lobby_display_name(name, player_word && player_word[0] ? player_word : lobby_own_text(LT_PLAYER, lang), nm, sizeof nm);
    return lobby_format(lobby_own_text(text, lang), nm, NULL, out, size);
}

const char *lobby_silent_text(const char *name, int seconds, int lang, char *out, size_t size)
{
    char secs[16];
    snprintf(secs, sizeof secs, "%d", seconds < 0 ? 0 : seconds);
    return fmt_fit(out, size, lobby_own_text(LT_PARTNER_SILENT, lang), name && name[0] ? name : "?", secs);
}
