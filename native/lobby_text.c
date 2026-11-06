/*
 * Texts of the co-op lobby. Most captions are the game's own strings (the game ships them in all 12 languages; lobby_stock_ids says where),
 * the English column of those entries is only the fallback for a table that is not loaded. The own strings (everything from LT_WAITING on)
 * are translated here. Column order of every row: English, French, Italian, German, Spanish, Russian, Polish, Brazilian Portuguese,
 * Japanese, Korean, Simplified Chinese, Traditional Chinese (the order of lobby_lang_names). A NULL column falls back to English.
 */
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
    [LT_WAITING] = {-1, 0}, [LT_PARTNER_JOINED] = {-1, 0}, [LT_ENTER_ADDR] = {-1, 0}, [LT_CONNECTING] = {-1, 0}, [LT_NO_FRIENDS] = {-1, 0},
    [LT_REFRESH] = {-1, 0}, [LT_NAME] = {-1, 0}, [LT_PING] = {-1, 0}, [LT_HINT] = {-1, 0}, [LT_HINT_IP] = {-1, 0}, [LT_YOUR_ADDR] = {-1, 0},
    [LT_STEAM_HINT] = {-1, 0}, [LT_STEAM_NA] = {-1, 0}, [LT_NEED_READY] = {-1, 0}, [LT_NOT_READY] = {-1, 0}, [LT_SEARCHING] = {-1, 0},
    [LT_CHARACTER] = {-1, 0}, [LT_STARTING] = {-1, 0}, [LT_YOU] = {-1, 0}, [LT_HOST_TAG] = {-1, 0}, [LT_PARTNER_READY] = {-1, 0}, [LT_LEAVE_HINT] = {-1, 0}, [LT_SAVE_WARN] = {-1, 0},
    [LT_WAIT_LOAD] = {-1, 0}, [LT_LEG_OK] = {-1, 0},
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
    /* ---- stock strings: English fallback, plus our own words where the game's own table holds English in a CJK language (menu.c env_text uses them instead of an all-ASCII stock string there) ---- */
    [LT_COOP] = {"Co-Op Play"}, [LT_HOST] = {"Find a Partner"}, [LT_JOIN] = {"Join a Partner"}, [LT_FRIEND] = {"Friend"}, [LT_LINK] = {"System Link"},
    [LT_BACK] = {"Back"}, [LT_READY] = {"Ready"}, [LT_START] = {"Start Game", NL, NL, NL, NL, NL, NL, NL, "ゲーム開始"},
    [LT_DIFFICULTY] = {"Difficulty", NL, NL, NL, NL, NL, NL, NL, "難易度", NL, "难度", "難度"}, [LT_EPISODE] = {"Episode", NL, NL, NL, NL, NL, NL, NL, "エピソード"},
    [LT_CLAIRE] = {"Claire"}, [LT_MOIRA] = {"Moira"}, [LT_BARRY] = {"Barry"}, [LT_NATALIA] = {"Natalia"}, [LT_PLAYER] = {"Player"}, [LT_PARTNER] = {"Partner"},
    [LT_YES] = {"Yes"}, [LT_NO] = {"No"}, [LT_QUIT_SESSION] = {"Quit session?"}, [LT_LEAVE] = {"Leave", NL, NL, NL, NL, NL, NL, NL, "退出"},
    [LT_SAME_SEL] = {"You cannot make the same selection as your partner."}, [LT_FAIL_JOIN] = {"Failed to join session."},
    [LT_FAIL_CREATE] = {"Failed to create a session."}, [LT_CONN_LOST] = {"Connection to the other player has been terminated."},
    [LT_CASUAL] = {"Casual"}, [LT_NORMAL] = {"Normal"}, [LT_HARD] = {"Hard"}, [LT_VERY_HARD] = {"Very Hard"}, [LT_INVITE] = {"Invite Friends"},

    /* ---- own strings ---- */
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
    [LT_SAVE_WARN] = {"Co-op uses the campaign save slot: checkpoints overwrite the current save.",
                      "Le coop utilise l'emplacement de sauvegarde de la campagne : les points de contrôle écrasent la sauvegarde actuelle.",
                      "La co-op usa lo slot di salvataggio della campagna: i checkpoint sovrascrivono il salvataggio attuale.",
                      "Koop nutzt den Speicherplatz der Kampagne: Checkpoints überschreiben den aktuellen Spielstand.",
                      "El modo cooperativo usa la ranura de guardado de la campaña: los puntos de control sobrescriben la partida actual.",
                      "Кооператив использует слот сохранения кампании: контрольные точки перезапишут текущее сохранение.",
                      "Tryb współpracy używa slotu zapisu kampanii: punkty kontrolne nadpiszą bieżący zapis.",
                      "O modo cooperativo usa o espaço de salvamento da campanha: os checkpoints substituem o salvamento atual.",
                      "協力プレイはキャンペーンのセーブスロットを使用します。チェックポイントで現在のセーブデータが上書きされます。",
                      "협동 플레이는 캠페인 저장 슬롯을 사용합니다. 체크포인트에서 현재 저장 데이터가 덮어쓰기됩니다.",
                      "协力游玩将使用战役存档位：检查点会覆盖当前存档。", "協力遊玩將使用戰役存檔位：檢查點會覆蓋目前存檔。"},
    [LT_WAIT_LOAD] = {"Waiting for %s to finish loading...", "En attente de %s : chargement en cours...", "In attesa che %s finisca il caricamento...",
                      "Warte darauf, dass %s fertig geladen hat...", "Esperando a que %s termine de cargar...", "Ожидание: %s ещё загружает уровень...",
                      "Czekam, aż %s skończy wczytywanie...", "Aguardando %s terminar de carregar...", "%sの読み込み完了を待っています...",
                      "%s님의 로딩이 끝나기를 기다리는 중...", "正在等待%s完成加载...", "正在等待%s完成載入..."},
    [LT_LEG_OK] = {"Confirm", "Valider", "Conferma", "Bestätigen", "Confirmar", "Подтвердить", "Zatwierdź", "Confirmar", "決定", "확인", "确认", "確認"},
};

const char *lobby_own_text(int text, int lang)
{
    if (text < 0 || text >= LT_COUNT) return "";
    if (lang < 0 || lang > 11) lang = 0;
    const char *s = T[text][lang];
    if (!s) s = T[text][0];
    return s ? s : "";
}
