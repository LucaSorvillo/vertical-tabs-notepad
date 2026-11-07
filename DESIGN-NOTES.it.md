# VTabsNotepad — note di progettazione

> Note di sviluppo dettagliate, in italiano, scritte man mano durante lo sviluppo: come funziona ogni parte e
> perché è fatta così. Alcune sezioni raccontano anche tentativi poi cambiati. Per l'uso c'è il
> [README](README.it.md); un riepilogo tecnico in inglese è in [ARCHITECTURE.md](ARCHITECTURE.md).

Estensioni per il **Blocco note di Windows 11** (`Microsoft.WindowsNotepad`).
Prima feature: **schede verticali** (pannello laterale), comode quando ci sono molte schede aperte.
Seconda: il pulsante **"Torna su"** nell'angolo in basso a destra dell'editor.

Non modifica né sostituisce il Blocco note su disco. È un piccolo "mod" in stile Windhawk:
un launcher con icona nella tray inietta una DLL nel processo del Blocco note e, usando l'API
ufficiale di diagnostica XAML di WinUI (`InitializeXamlDiagnosticsEx`, la stessa che usa Visual
Studio per ispezionare la UI), aggiunge l'interfaccia riusando componenti, dati e comandi nativi.

## Come funziona

Il Blocco note moderno è un guscio nativo Win32 che ospita più "isole" XAML (barra schede, menu,
barra di stato, barre di scorrimento) più un editor `RichEdit` su un thread separato. Un solo
processo gestisce tutte le finestre.

1. **Launcher** (`VTabsNotepad.exe`): sta nella tray, rileva i processi del Blocco note e inietta
   `VTabsNotepadMod.dll`. Dal menu si attivano/disattivano le feature (scritte in `settings.ini`).
2. **Mod** (`VTabsNotepadMod.dll`): si aggancia all'albero XAML e, per ogni finestra, individua la
   `TabsBar` (il controllo schede del Blocco note, con API pubblica nel suo `NotepadXamlUI.winmd`).
   - Il pannello verticale è un controllo nostro fatto di componenti WinUI di sistema (`ListView`,
     `TextBox`, pennelli del tema), ospitato in una **nostra isola XAML** (`DesktopWindowXamlSource`)
     e legato ai dati nativi: elenco, selezione, titolo, stato "modificato", percorso nel tooltip.
   - La barra orizzontale originale resta viva (è il "motore" dei documenti) ma invisibile; chiudere
     o aprire una scheda dal pannello preme i suoi pulsanti nativi via UI Automation, quindi vale la
     stessa logica del Blocco note, compreso "Salvare le modifiche?".
   - Riordino: la riga trascinata si sposta dal vivo nel pannello e al rilascio lo stesso spostamento
     passa alle schede vere con `TabsBar.ReorderTab`; il Blocco note lo salva nella sessione e lo usa
     per i suoi comandi (es. "Chiudi schede a destra").
   - Menu contestuale: è quello del Blocco note (`TabsBarItemContextFlyout`), aperto sulla vera scheda
     nativa; la striscia nascosta viene portata per un attimo sotto il puntatore (invisibile, in fondo
     all'ordine Z) così il menu compare esattamente dove si è cliccato, poi torna fuori vista. Solo
     quando lo apre il pannello, in fondo compare "Colore scheda", tolto alla chiusura del menu.
   - Sfondo: la finestra del Blocco note usa la Mica Alt di sistema (`DWMWA_SYSTEMBACKDROP_TYPE` =
     "finestra a schede"); il pannello usa lo stesso materiale (`MicaBackdrop`, tipo `BaseAlt`)
     dentro la propria isola, quindi resta opaco e coincide con la barra del titolo.
   - Barra dei menu (`NotepadXamlUI.MainMenuBar`, in un'isola propria): normalmente ha il colore
     dell'editor, un velo `LayerOnMicaBaseAltFillColorDefault` sul contenitore `RootContainer`. Con il
     pannello attivo il velo diventa trasparente e si vede il materiale della finestra, come nella
     barra del titolo; senza pannello torna com'era.
   - Colori: legati al `TabId` della scheda, che è lo stesso identificativo della sessione del Blocco
     note (`TabState\<id>.bin`); i colori delle schede chiuse per sempre vengono ripuliti all'avvio.
   - `WindowLayout` ricava la colonna laterale: per ogni finestra figlia ricorda il rettangolo che le
     dà il Blocco note e lo trasla fuori dalla colonna; si riallinea su `WM_SIZE`, eventi WinEvent e
     con un controllo periodico. Libera anche la zona della barra del titolo occupata dalle schede
     nascoste, così torna trascinabile.
   - **Colonna in cima alla finestra** (il pannello è sempre a sinistra, come le schede verticali di Chrome): la
     colonna va dalla cima della finestra al fondo dell'editor e anche la barra dei menu comincia dal suo
     bordo destro (la barra di stato, sotto, resta larga quanto la finestra). La riga del titolo, rimasta
     vuota con le schede nella colonna, **si nasconde** (come Edge con "nascondi la barra del titolo"): la
     barra dei menu sale lì (3 px sotto la cima: il bordo superiore ridimensiona ancora) e finisce prima dei
     pulsanti della finestra, lasciando loro 154 DIP (tre pulsanti da 46 DIP più 16 liberi, da cui la
     finestra si trascina); l'editor e la sua barra di scorrimento verticale si allungano verso l'alto, e con
     loro la superficie del testo dentro l'editor. Il Blocco note però dimensiona le sue barre di
     scorrimento sul proprio layout (isola verticale non allungata, isola orizzontale senza la colonna) e le
     reimposta a ogni suo ricalcolo: `features/ScrollBarFit.cpp` aggiunge a vista e contenuto di ciascuna la
     differenza della sua isola e li riscrive subito ogni volta che lui li cambia (callback sulle proprietà),
     così la verticale arriva sempre fino in fondo, l'orizzontale non sfora a destra e la corsa del testo
     resta la sua. In cima alla colonna, alta quanto la barra dei menu, c'è la fascia del titolo con
     comprimi/espandi, allineato ai menu; separatore e divisore
     cominciano dove comincia l'editor. Il mouse delle isole XAML non passa da
     `WM_NCHITTEST` (`HTTRANSPARENT` non basta: verificato in VTabsExplorer), quindi l'isola del pannello ha
     una **regione di finestra** che esclude la fascia salvo i pulsanti (rifatta dopo ogni layout, solo se
     cambia): lì sotto c'è la finestra del Blocco note, che risponde come la sua barra del titolo
     (trascinamento, doppio clic per ingrandire, menu di sistema, bordo superiore che ridimensiona). La
     regione taglia anche il disegno dell'isola e nel buco si vede la finestra sotto: il Blocco note ci
     disegna la sua icona, sotto "comprimi" (ne spuntava il bordo inferiore), per questo i pulsanti si
     tengono fino al fondo della fascia. Lo stesso vale per la **barra dei menu salita**: anche la sua isola si
     prendeva il mouse nelle zone vuote (tra "Visualizza" e "Novità" non si trascinava la finestra). Ora la sua
     regione ha un buco negli spazi tra i comandi, e lì la finestra risponde come barra del titolo. I buchi si
     calcolano dall'albero XAML della barra:
     - comandi: il `MenuBar`, i pulsanti, le caselle e gli altri controlli raggiungibili con il tabulatore, quindi
       anche la barra di formattazione o il pulsante dell'account quando compaiono;
     - spazi di almeno 16 DIP, a 4 DIP dai comandi;
     - fino a 1 DIP dal fondo, per non tagliare la linea sottile sotto la barra.
     Si ricalcolano dopo ogni layout dell'isola e valgono solo se la sua misura corrisponde, così durante un
     ridimensionamento non finiscono sopra un pulsante spostato. L'opzione "Pannello a destra" non c'è più (tolta il
     2026-11-04): in alto a destra ci sono i pulsanti della finestra, quindi lì la colonna non poteva salire.
     Le isole che il Blocco note fa **galleggiare sopra l'editor** (la barra di ricerca di Ctrl+F, in cima e centrata)
     non si stringono né si allungano come l'editor: si spostano con lui alla loro misura, perché il loro contenuto è
     impaginato per quella (vedi "Stato", 2026-11-06).
     La finestra principale del Blocco note **non ha `WS_CLIPCHILDREN`**: quando ridipinge il proprio sfondo
     copre anche l'editor (le isole XAML sono composte sopra e non ne risentono), che di solito poi si
     ridisegna, ma sta su un altro thread e a volte lo fa prima. Con l'editor salito, dopo un cambio di layout
     (pannello compresso/espanso) la finestra ridipingeva la sua riga del titolo, (0, 0)–(inizio dei pulsanti
     della finestra, fondo della riga originale), e in cima all'editor restava una fascia scura di 8 px.
     Il subclass della finestra principale legge la regione da ridipingere prima di `WM_PAINT` (e l'area del
     DC in `WM_ERASEBKGND`) e, dopo, invalida la parte dell'editor che ha coperto (solo `RDW_INVALIDATE`,
     niente chiamate sincrone verso il thread dell'editor).
     **Zona vetro** (`Glass.cpp`): il Blocco note estende la cornice di sistema nell'area client
     (`DwmExtendFrameIntoClientArea`) fino al fondo della barra dei menu, così dietro titolo e menu si vede la Mica
     Alt. Lì DWM compone i pixel con il loro alfa e l'editor, che disegna senza alfa, si somma allo sfondo: la
     parte salita era una **fascia più chiara** (`#272727` + `#202020` = `#474747`; con lo sfondo dell'editor
     magenta diventava `#FF20FF`). `Notepad.exe` importa la funzione direttamente e la richiama quando la finestra
     viene ingrandita o ripristinata (93 e 91 px al 125%). Un aggancio nella sua tabella delle importazioni ricorda
     ogni richiesta e, con la riga del titolo nascosta, ne limita il margine superiore all'inizio dell'editor
     (44 px). Quando la colonna si spegne torna la zona del Blocco note. La DLL non viene mai scaricata,
     quindi l'aggancio non va tolto.
     **Pulsanti della finestra alti quanto la riga** (`CaptionButtons.cpp`, lo stesso componente di VTabsExplorer, dove
     è descritto in dettaglio): con la riga del titolo nascosta riduci/ingrandisci/chiudi vanno dalla cima della
     finestra al fondo della barra dei menu salita (44 px al 125%, 35 da ingrandita), come in Chrome e nel Terminale
     di Windows; quelli di DWM (30 DIP) si nascondono con il flag non documentato `0x10000` delle opzioni del tema
     della zona non client (quello del Windows App SDK) e tornano togliendolo. Una finestra figlia layered disegnata
     con Direct2D: glifi di Segoe Fluent Icons identici a quelli di DWM, centrati sulla riga dei menu (come
     l'icona delle impostazioni), colori dei pulsanti di Windows 11, hit test `HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`,
     menu di sistema col tasto destro, suggerimenti. Il tema è quello del Blocco note (che lo dà a DWM:
     `DWMWA_USE_IMMERSIVE_DARK_MODE`), riletto quando cambia. Senza editor (pagina Impostazioni) il layout si toglie
     e tornano i pulsanti di DWM.

Nel pannello: clic per cambiare scheda (il focus torna all'editor), X o clic centrale per chiudere,
`Canc` chiude la scheda selezionata; il "+" (nuova scheda, solo icona) sta subito sotto l'ultima
scheda e, quando le schede riempiono il pannello, resta visibile in fondo mentre l'elenco scorre.
Lo sfondo è la stessa Mica Alt della barra del titolo (con la trasparenza di Windows spenta, il suo
colore pieno), e con il pannello attivo anche la barra dei menu prende quel colore: resta più chiaro
solo l'editor. Ogni scheda è una "card" con gli angoli arrotondati; la scheda attiva ha il fondo un
po' più deciso, un bordino, la stanghetta a sinistra nel colore della scheda (grigio neutro se non ne
ha uno) e il titolo più spesso. I titoli sono identici a quelli delle schede orizzontali: carattere,
dimensione, peso e colore vengono letti dalle schede native (Segoe UI Variable a 12; normale peso 400
in grigio `TextFillColorSecondary`, attiva peso 600 in bianco; al passaggio del mouse il colore non
cambia, come nell'originale). A pannello compresso ogni scheda è un'icona con l'iniziale
del titolo (come la favicon delle schede del browser); da espanso le righe mostrano solo il titolo.
A destra del titolo, come nelle schede native, la scheda con modifiche non salvate mostra il pallino (anche
quella attiva, dalla 1.2.2) e passandoci sopra con il mouse la X; la scheda attiva senza modifiche mostra
sempre la X.

- **Comprimi/espandi** (pulsante in alto a sinistra, come le schede verticali di Chrome/Edge): da
  compresso ogni scheda diventa un'icona con l'iniziale del titolo, con il pallino se è modificata. Lo
  stato è salvato e vale per tutte le finestre. La ricerca nelle schede (lente e casella "Cerca nelle
  schede") è stata tolta il 2026-11-05, su richiesta dell'utente.
- **Colore della scheda**: clic destro > "Colore scheda" (Grigio, Blu, Rosso, Giallo, Verde, Rosa,
  Viola, Azzurro, Arancione, Nessuno). L'intera riga prende una tinta tenue di quel colore (15% sopra
  lo sfondo del pannello, 27% se è la scheda attiva); le schede senza colore hanno un velo neutro
  appena visibile. Da compresso anche l'icona della scheda lo prende pieno. I colori sono salvati in
  `%LOCALAPPDATA%\VTabsNotepad\tab-colors.ini` e restano anche dopo il riavvio del Blocco note.

- **Riordino**: trascinare una riga in su o in giù (come le schede orizzontali); vicino ai bordi
  dell'elenco scorre da solo, `Esc` annulla.
- **Clic destro** (o `Maiusc+F10` / tasto Menu) su una riga: il menu nativo della scheda (Nuova
  scheda, Chiudi scheda, Chiudi altre schede, Chiudi schede a destra; "a destra" = sotto).
- **Larghezza**: trascinare la linea divisoria (il cursore diventa ↔, la linea si colora); doppio clic
  per tornare alla larghezza predefinita. Viene salvata e applicata a tutte le finestre.

### Pulsante "Torna su"

Appena il documento scorre verso il basso, nell'angolo in basso a destra dell'editor (accanto alla barra
di scorrimento, sopra quella orizzontale se c'è) compare sfumando un pulsante tondo con la freccia in
su; il clic riporta all'inizio del documento con uno scorrimento animato di circa un quarto di secondo e
restituisce il focus all'editor (il cursore resta dov'era). In cima al documento il pulsante sparisce.
Segue la scheda visibile (ogni documento ha la sua posizione), il pannello delle schede e la finestra;
il testo resta visibile intorno al cerchio. Si accende e si spegne dal menu della tray
(`[BackToTop] Enabled` in `settings.ini`), indipendentemente dalle schede verticali.

Come funziona (`features/BackToTop.cpp`): la barra di scorrimento verticale del Blocco note è uno
`ScrollViewer` XAML nella propria isola, sincronizzato con l'editor nei due sensi. Il suo evento
`ViewChanged` dice quando il documento scorre (nessun controllo continuo), e farlo scorrere riporta in
cima l'editor. Il pulsante sta in una nostra isola XAML sopra l'editor, `WS_EX_LAYERED` come l'isola della
barra di scorrimento del Blocco note: è trasparente intorno al cerchio (nessuna immagine fantasma, a
differenza del pannello).

L'isola resta **mostrata** ("parcheggiata") finché c'è un editor e la finestra non è ridotta a icona. Comparsa
e scomparsa del pulsante cambiano solo l'opacità, e a pulsante nascosto l'isola è trasparente al mouse
(`WS_EX_TRANSPARENT`): clic e rotella vanno al testo sotto, e a schermo non si vede nulla (colore uguale allo
sfondo dell'editor). Prima l'isola si mostrava al primo scatto sotto la cima e si nascondeva tornando su. Ma
mostrare o nascondere una finestra fa generare a Windows un `WM_MOUSEMOVE` per la finestra sotto il cursore,
cioè l'editor. Il Blocco note lo prendeva per un mouse vero e passava la barra di scorrimento all'indicatore
"da mouse", più corto e più in basso: il cursore della barra cambiava forma e grandezza proprio mentre si
scorreva. Cambiare lo stile esteso, invece, non genera nessun movimento.

### Riapri la scheda chiusa (Ctrl+Maiusc+T)

Come nel browser: la scheda chiusa per sbaglio torna **al posto che aveva** nell'elenco, con il suo colore (dalla
1.2.0), e premendo di
nuovo si va a ritroso (le ultime 25). Come in Chrome la cronologia **resta anche chiudendo la finestra o il
Blocco note**: si chiude una scheda, si chiude la finestra, si riapre il Blocco note e Ctrl+Maiusc+T la
riporta. Vale in qualunque finestra. Vale per le schede di file salvati: di ognuna si ricordano solo il
percorso e la posizione, in `%LOCALAPPDATA%\VTabsNotepad\closed-tabs.txt` (una riga "posto, tabulazione,
percorso" per scheda, più tabulazione e colore se ne aveva uno; mai il contenuto). Una scheda **mai salvata** non si può riaprire e al comando non
succede nulla — a quella ci pensa la richiesta di salvataggio che il Blocco note fa già quando la si
chiude. Si accende e si spegne dal menu della tray (`[ReopenTab] Enabled`), indipendentemente dalle schede
verticali; **spegnendola il file si cancella**.

Chiudendo una finestra il Blocco note toglie le sue schede dall'elenco circa 150 ms prima di distruggerla. Non
diventano "chiuse", perché la conferma chiede mezzo secondo; per l'ultima finestra tornano comunque con la
sessione. Ctrl+Maiusc+T salta le schede già aperte nella finestra, per esempio quelle tornate con la sessione.
Il file si rilegge a ogni chiusura e a ogni pressione, sotto un mutex con nome condiviso da finestre e
istanze, quindi non c'è una copia in memoria da tenere allineata. La suite mette da parte la cronologia
dell'utente durante le prove (`Enter-ReopenIsolation`/`Exit-ReopenIsolation` nel TestKit) e alla fine la
rimette, con in coda le schede che l'utente avesse chiuso nel frattempo.

Come funziona (`features/ReopenTab.cpp`): la combinazione si chiede a Windows con `RegisterHotKey`
**solo mentre quella finestra è attiva** (`WM_ACTIVATE` in un nostro subclass, più un controllo di
sicurezza nel timer da 500 ms) e si restituisce appena perde il fuoco: così resta libera per gli altri
programmi che la usano (nei browser, per esempio) e della tastiera non si legge nient'altro, perché
Windows ci consegna soltanto questa combinazione. Le chiusure si riconoscono dalle schede che spariscono
da `TabItems` (il percorso è il `Tooltip` della scheda): un riordino le fa sparire e ricomparire, quindi
una chiusura si considera vera solo se la scheda non torna entro mezzo secondo. Sul thread UI non si
guarda mai il disco — una cartella di rete non raggiungibile bloccherebbe la finestra per secondi:
basta che la descrizione della scheda sia un percorso assoluto, e che il file esista si controlla sul
thread che lo riapre. Il file si riapre
lanciando il Blocco note con quel file, come un doppio clic: è la stessa istanza, che lo mette come
scheda della finestra attiva (in fondo); appena compare, la scheda torna al suo posto con
`TabsBar.ReorderTab`, lo stesso metodo del riordino col mouse.

Limiti noti: la riapertura passa dall'apertura normale del file, quindi segue l'impostazione del Blocco
note "Apertura dei file": con "in una nuova finestra" la scheda chiusa torna in una finestra nuova
invece che al suo posto (con l'impostazione predefinita, "in una nuova scheda", torna al suo posto).
Con **due finestre** del Blocco note, passando dall'una all'altra la combinazione può
restare di nessuno per meno di mezzo secondo (ogni finestra ha il suo thread, quindi il rilascio della
prima e la richiesta della seconda non sono ordinati; al controllo successivo si sistema). Se il file
della scheda chiusa è già aperto in un'**altra** finestra, il Blocco note porta in primo piano quella
invece di rimetterlo qui.

### Barre di scorrimento subito larghe sotto il mouse

Come in Esplora file: appena il mouse va su una barra di scorrimento del Blocco note, la barra si allarga
(binario e frecce). Senza la mod lo fa solo dopo 0,4 s: è il modello della `ScrollBar` di WinUI 3, che
nello stato "Expanded" (gruppo `ConsciousStates`, scelto subito da `ScrollBar::ChangeVisualState` quando
il mouse entra) fa partire tutte le animazioni con `BeginTime` = `ScrollBarExpandBeginTime` = 0,4 s.
Esplora file usa le barre di DirectUI, che non hanno quel ritardo. Vale per tutte le barre della finestra:
editor, menu, elenchi, il nostro pannello. È **sempre attiva**, indipendentemente dalle schede verticali:
l'opzione nella tray (`[ScrollBars] FastHover`) è stata tolta il 2026-11-05 su richiesta dell'utente.

Come funziona (`features/ScrollBarHover.cpp`): la diagnostica XAML segnala ogni `ScrollBar` che nasce
nel Blocco note; il timer da 500 ms di ogni finestra prende quelle del suo thread che hanno già il
modello e mette a 0 il `BeginTime` delle animazioni di "Expanded" ed "ExpandedWithoutAnimation",
ricordando i valori originali per stato e posizione nello storyboard (un `weak_ref` alle animazioni non
basta: WinUI ne rilascia l'involucro e il riferimento scade). Se la finestra esce dalla mod (modalità
sviluppo, `dev-windows.txt`) si ripercorre il modello di ogni barra e tornano i ritardi di sistema. Durata
dell'animazione (0,17 s) e ritardo prima di stringersi quando il mouse esce (0,5 s) restano quelli di
sistema.

Misurato con il mouse vero sulla barra verticale della finestra di prova (`Measure-VScrollHover`):
senza la funzione la barra è ancora sottile a 340 ms e si allarga a 480-650 ms; con la funzione è già
larga a 90 ms e completa a 200 ms.

### Disattiva lo scorrimento fluido

Con la rotella sul testo il documento si sposta **subito** delle righe impostate in Windows ("righe per
scatto", qui 2), come nel vecchio Blocco note, invece di scorrere a piccoli passi. Senza la mod ogni scatto
dura circa 0,3 s: una decina di passi da ~30 ms. Le righe percorse sono le stesse, cambia solo la velocità.
Ctrl+rotella resta lo zoom del Blocco note e Maiusc+rotella resta com'è. Tastiera (PagGiù, frecce),
rotella orizzontale e trascinamento della barra erano già istantanei. Con la rotella sopra la barra di
scorrimento lavora la barra XAML del Blocco note, quasi istantanea; lì non si interviene. Opzione nella
tray "Disattiva lo scorrimento fluido" (`[SmoothScroll] Disabled`, attiva di base), indipendente dalle
schede verticali.

Come funziona (`features/WheelScroll.cpp`): l'editor è la RichEdit di Office (`riched20.dll` 16.0 nel
pacchetto del Blocco note, senza simboli pubblici). Lo scorrimento fluido è suo, sempre acceso: tra le
impostazioni di sistema chiede solo allineamento dei menu, smussatura dei caratteri, contrasto elevato,
metriche, mouse e righe per scatto, non quelle di animazione. L'editor di ogni scheda
(`NotepadTextBox` > `RichEditD2DPT`) vive su un **thread suo**, e su ciascuno la mod mette un hook
`WH_GETMESSAGE`. Il timer da 500 ms della finestra aggancia anche le schede aperte dopo e sgancia quelle
chiuse. Lo scatto (`WM_MOUSEWHEEL` verso la superficie di testo, senza Ctrl né Maiusc) si trasforma sul
posto, prima che il Blocco note lo veda, in uno scorrimento immediato fatto dall'hook: `EM_LINESCROLL` delle
righe di Windows ("una schermata alla volta" = `WM_VSCROLL SB_PAGEUP/SB_PAGEDOWN`). Se l'editor non si muove,
la stessa distanza a pixel con `EM_SETSCROLLPOS`, misurando l'altezza di riga con `EM_POSFROMCHAR`, ma **mai oltre
la fine vera del testo**: `nMax - nPage` della barra della superficie, dove si fermano `SB_BOTTOM`, le righe e la
rotella nativa (0 se il testo ci sta; nessun ripiego se la corsa è sconosciuta o fasulla). `EM_SETSCROLLPOS`, a
differenza delle righe, accetta qualsiasi posizione. Senza questo limite, in fondo al documento ogni scatto portava
il testo nel vuoto (fino a 179 px oltre la fine con 5 scatti), e così anche in un documento che ci sta in altezza
(da y 0 a 256). Il Blocco note poi lo riportava indietro a scatti. Se nemmeno così si muove (in fondo, per
esempio), lo scatto passa al Blocco note com'era, che lì resta fermo; la rotella non resta mai senza effetto per colpa
nostra. La posizione si legge con `EM_GETSCROLLPOS`, che si aggiorna subito. Le frazioni di scatto (touchpad,
rotelle ad alta risoluzione) si accumulano finché non fanno una riga, e cambiando verso il resto si azzera.

### Corsa verticale fasulla dell'editor (correzione di un difetto del Blocco note)

Difetto del Blocco note, presente anche senza la mod. Si manifesta con un documento con righe più larghe della
vista ("a capo automatico" spento, serve la barra orizzontale) che passa dal "non ci sta in altezza" al "ci
sta". Allora la RichEdit mette come **massimo della barra verticale quello orizzontale** (la larghezza del
testo) e smette di aggiornarne la pagina. La barra XAML del Blocco note mostra una corsa che non esiste: la
rotella non fa nulla e trascinando la barra il testo non si muove, perché l'editor sa che il testo ci sta.

Con la mod capita più spesso: la colonna in cima alza l'editor di ~49 px, e documenti che nella vista originale
non ci stavano per poco ora ci stanno. Caso dell'utente: la scheda "FASE 0", 49 righe da 191 caratteri, 906 px
di testo, vista originale da 889 px e vista della mod da 938. Riprodotto nel Blocco note senza mod allungando
la finestra: massimo verticale 920 → 1593, uguale all'orizzontale.

Correzione (`features/ScrollRangeFix.cpp`, sempre attiva): ogni 100 ms, con un timer proprio, due
`GetScrollInfo` sull'editor visibile, senza messaggi. Se il massimo verticale è uguale all'orizzontale, con una
corsa verticale e la barra orizzontale necessaria, si conferma che il testo ci sta davvero: fondo dell'ultima riga
con una posizione valida, chiesto all'editor sul suo thread con `SendMessageTimeout` (al massimo ogni 2 s per lo
stesso stato). I buffer stanno nello heap: se il tempo scade non si liberano. Confermato, si azzera la corsa
verticale con `SetScrollInfo`. La barra XAML segue e sparisce, e riaccorciando la finestra torna la corsa vera.

### Barra rimasta con la corsa vecchia (correzione di un altro difetto del Blocco note)

Anche questo c'è senza la mod. Le barre XAML del Blocco note rileggono le corse dell'editor solo quando la RichEdit
lo avvisa che sta per ridisegnarsi: `WM_COMMAND EN_UPDATE` al genitore `NotepadTextBox`, cioè quando si scorre o si
scrive. La RichEdit però impagina i documenti in background e allunga la corsa più tardi senza ridisegnare nulla.
Con 300 righe aperte il massimo passa da 2108 a 5438 px in ~0,4 s. La barra resta con il cursore della corsa
vecchia (45% della barra invece del 17,6%) e al **primo scatto di rotella il cursore cambia di colpo grandezza e
posizione**. Succede aprendo un file, sulle schede ripristinate all'avvio e quando cambia la pagina dell'editor
senza un ridisegno. Senza mod il cursore restava al 44% per 5 s e saltava al 16,7% al primo scatto.

Correzione, nello stesso controllo da 100 ms: se massimo o pagina dell'editor visibile (verticale o orizzontale)
sono cambiati dal controllo precedente, o è cambiato l'editor, si manda al genitore lo stesso `EN_UPDATE`
(`PostMessage`). Il Blocco note rilegge le corse e aggiorna le barre. Provato prima di sceglierlo:

- il testo non si muove;
- la scheda non risulta modificata e il titolo non cambia;
- l'indicatore della barra non compare (0 fotogrammi su 8).

L'alternativa, scorrere di 1 px avanti e indietro, aggiornava la barra ma la faceva lampeggiare in 8 fotogrammi su 8;
`EN_VSCROLL`/`EN_HSCROLL` finti non facevano nulla. Ora la barra è giusta circa 0,1 s dopo che la RichEdit ha finito
di impaginare, senza scorrere. Nella corsa fasulla nessun ciclo: una correzione e un solo `EN_UPDATE` in 4 s.

La barra di WinUI ha due indicatori. Quello "di scorrimento" è sottile e lungo quanto tutta la barra. Quello "da
mouse" lascia lo spazio delle frecce, quindi è più corto e più in basso: 152 contro 138 px senza mod. Lo
`ScrollViewer` passa al secondo quando il mouse si muove sull'editor o passa sopra la barra; questo resta, con o
senza mod. Con la mod però succedeva anche al primo scorrimento sotto la cima, per colpa dell'isola di "Torna su"
mostrata in quel momento: vedi "Pulsante Torna su". Misurato a ogni scatto, fotogramma per fotogramma e senza
toccare il mouse: prima "da mouse" alla prima prova e "di scorrimento" alle seguenti; ora "di scorrimento" in 6
prove su 6, come senza mod.

### Clic che riattiva la finestra (il testo saltava; corretto nella 1.2.1)

Tornando al Blocco note da un'altra app con un clic su una riga, il testo saltava e il clic finiva su un'altra riga,
oppure selezionava invece di posare il cursore. Senza la mod non succede.

A ogni attivazione della finestra il Blocco note rimette le sue finestre figlie dove le vuole lui: editor, superficie
di testo, barre di scorrimento, barra dei menu. Lo fa dentro `WM_ACTIVATE`, anche senza cambiare misura (niente
`WM_SIZE`). La mod le riportava nel suo layout solo all'arrivo dell'evento WinEvent, 30-60 ms dopo. Il clic che aveva
attivato la finestra arrivava all'editor proprio in quel momento, cioè ~49 px più in basso: cadeva tre righe più su
e la selezione andava da lì al punto vero. In più la superficie di testo, ridimensionata due volte con il focus
mentre elaborava il clic (un `WM_LBUTTONDOWN` inviato durante `WM_MOUSEACTIVATE`), riportava in vista il cursore
lampeggiante prima di posare il clic: il testo saltava alla sua riga.

Correzione (`PositionPin.cpp`): per ogni finestra che il layout sposta si ricordano il rettangolo naturale e quello
voluto. Un subclass sul thread della finestra trasforma in "nessuno spostamento" ogni `WM_WINDOWPOSCHANGING` che chiede
di nuovo proprio il rettangolo naturale mentre la finestra è già dove la vuole la mod. Gli editor hanno un thread
ciascuno: il subclass lo si installa da lì, con un hook `WH_GETMESSAGE` e un messaggio registrato. Un rettangolo
naturale nuovo (finestra ridimensionata) passa come prima. In più `Check()` gira subito dopo `WM_NCACTIVATE`,
`WM_ACTIVATE` e `WM_WINDOWPOSCHANGED`.

Il clic sulla riga del titolo nascosta (dove `HTCAPTION` lo dà la mod) apriva subito il ciclo di spostamento della
finestra. Alla sua uscita l'input di Windows chiede all'editor dove sta il cursore (`WM_IME_REQUEST`,
`IMR_QUERYCHARPOSITION`) e la RichEdit lo riporta in vista. Lo fa anche il Blocco note originale, cliccando o
trascinando la sua barra del titolo. Ora la finestra passa in primo piano alla pressione e si sposta solo
trascinando (`DragDetect`): il clic semplice non muove il testo.

Provato su una finestra di prova con un'altra app accanto, mouse e rotella veri, e confrontato con il Blocco note
senza mod (la stessa finestra esclusa con `dev-windows.txt`):

- clic su una riga dopo aver scorso con la rotella, da finestra attiva e non attiva;
- ritorno con Alt+Tab;
- clic e trascinamento sulla riga del titolo.

Prima 4 casi su 6 sbagliati, ora tutti giusti. Il layout resta giusto ridimensionando e ingrandendo la finestra,
cambiando scheda, comprimendo la colonna, con la barra di ricerca e con "Torna su".

Impostazioni e log: `%LOCALAPPDATA%\VTabsNotepad\` (`settings.ini`, `tab-colors.ini`, `logs\`).

## Compilazione

Requisiti: Visual Studio con strumenti C++ x64, Windows SDK (con C++/WinRT), CMake + Ninja
(inclusi in Visual Studio). Le proiezioni C++/WinRT vengono generate dai `.winmd` installati
(Windows SDK, Windows App SDK e `NotepadXamlUI.winmd` del Blocco note); dal pacchetto NuGet
`Microsoft.Web.WebView2` si scaricano solo i metadati richiesti da WinUI. Nessun file Microsoft
viene copiato nel repo.

```powershell
.\build.ps1                  # Release in build\bin
.\build.ps1 -Config Debug
.\build.ps1 -Clean           # ricompila da zero
```

Output: `build\bin\VTabsNotepad.exe` e `build\bin\VTabsNotepadMod.dll` (devono stare nella stessa cartella).

## Uso

```powershell
build\bin\VTabsNotepad.exe             # avvia la tray
build\bin\VTabsNotepad.exe --inject    # inietta nei Blocco note già aperti ed esce
build\bin\VTabsNotepad.exe --reopen-tab # riapre la scheda chiusa nella finestra attiva (come Ctrl+Maiusc+T)
build\bin\VTabsNotepad.exe --exit      # chiude la tray come "Esci" (la mod si spegne nei Blocco note aperti)
build\bin\VTabsNotepad.exe --exit --keep-mods  # chiude la tray lasciando la mod accesa (compilazione, prove)
```

Dal menu della tray: schede verticali on/off, pulsante "Torna su" on/off, "Riapri la scheda chiusa con Ctrl+Maiusc+T" on/off,
"Disattiva lo scorrimento fluido" on/off, avvio con Windows, cartella impostazioni/log, esportazione dell'albero XAML
(diagnostica). Le barre di scorrimento subito larghe sotto il mouse sono sempre attive (senza voce).

## Sviluppo e test

Il Blocco note è a **istanza singola** e all'avvio **ripristina la sessione precedente**. Per
provare una nuova build serve quindi un processo nuovo (chiudendo il Blocco note), e l'istanza di
prova conterrà le proprie schede: `tools\dev-test.ps1` agisce solo sulla finestra di prova (gate
in `%LOCALAPPDATA%\VTabsNotepad\dev-windows.txt`), apre una scheda vuota prima degli screenshot,
non chiude né modifica le schede esistenti e ripristina le impostazioni a fine test.

```powershell
.\tools\dev-test.ps1 -Start -OutDir C:\temp\npx   # con il Blocco note chiuso
.\tools\dev-test.ps1 -Shot -Name prova
.\tools\dev-test.ps1 -Cleanup
```

Se `dev-windows.txt` resta per sbaglio, la tray lo segnala ("modalità sviluppo") e offre una voce
per rimuoverlo.

### Test automatici dal vivo

`tools\test\Run-Tests.ps1` crea una **finestra del Blocco note dedicata** (File > Nuova finestra) con
file di prova in `%TEMP%\npx-test` e verifica pannello, selezione, sfondo di pannello e barra dei menu
(confrontando i pixel con la barra del titolo), colonna in cima alla finestra (dalla cima al fondo
dell'editor, barra dei menu ed editor dal suo bordo; nella fascia del titolo, fuori dai pulsanti, sotto il
punto c'è la finestra del Blocco note, che risponde `HTCAPTION`, mentre i pulsanti restano del pannello:
`WindowFromPoint`, che rispetta le regioni come il mouse vero), riga del titolo nascosta (barra dei menu
nella riga del titolo e fino ai pulsanti della finestra, menu allineati ai pulsanti della colonna, superficie
del testo alta quanto l'editor salito, pulsanti della finestra nostri alti quanto la barra dei menu con quelli di
DWM nascosti e primi nell'ordine Z, riduci/ingrandisci/chiudi su tutta la riga e tratto libero trascinabile, bordo in
cima che ridimensiona; zona vuota della barra dei menu trascinabile: tra "Visualizza" e "Novità" sotto il punto c'è la
finestra, che risponde `HTCAPTION`, mentre "Visualizza" e "Novità" restano della barra, anche da compresso; la finestra principale ridipinta da sola, `RedrawWindow` con `RDW_NOCHILDREN`, non lascia coperto
l'editor, e dopo comprimi/espandi in cima all'editor non resta una fascia scura: pixel a sinistra, sotto la riga del
titolo originale, uguali a quelli a destra, sotto i pulsanti della finestra; niente fascia chiara della zona vetro:
in cima all'editor lo stesso colore che a metà, con la finestra normale, ingrandita e ripristinata), barra di
ricerca (Ctrl+F, con "Modifica > Trova"): intera, cioè tutti i suoi pulsanti e caselle dentro la sua isola, centrata
sull'editor, anche in modalità "Sostituisci", col pannello compresso e in una finestra stretta, dove sporge sulla
colonna senza fascia nera in cima all'editor; poi il pannello di nuovo sopra l'editor nell'ordine Z. Niente
ricerca nel pannello (tolta), pannello compresso, colori delle
schede e aspetto della scheda attiva (colori attesi calcolati e misurati sullo schermo), menu contestuale,
coerenza dopo il riordino e pulsante "Torna su" (assente in cima, compare scorrendo, nell'angolo
dell'editor, il clic riporta alla riga 1 e il focus all'editor, segue la scheda visibile e il pannello
compresso/espanso, si spegne e si riaccende dalle impostazioni) e "riapri la scheda chiusa" (senza
premere tasti davvero: che la combinazione sia riservata si verifica provando a registrarla dal test —
mentre la tiene il Blocco note non ci riesce — e l'azione si chiede con `--reopen-tab`, lo stesso
comando della scorciatoia; si controlla che una scheda chiusa torni al suo posto esatto, che due schede
tornino a ritroso, che una scheda mai salvata non venga ricordata e che l'opzione la spenga e la
riaccenda); con `-RealInput` usa anche il mouse reale (fascia del titolo in cima alla colonna: la finestra
si trascina, il doppio clic la ingrandisce con la colonna sempre in cima e poi la ripristina; trascinamenti,
anche da compresso, scorrimento automatico, divisore, clic destro) e controlla che il
Blocco note salvi il nuovo ordine nella sessione. Con `-RealInput` le schede di prova cambiano strada facendo
(il clic destro chiude `npx-test-2`, lo scorrimento automatico aggiunge 20 schede vuote): la parte "riapri la
scheda chiusa" riapre prima `npx-test-2`, e il confronto dei colori dei titoli sullo schermo usa solo
schede orizzontali visibili.
Le altre schede non vengono toccate né stampate; alla fine la finestra di prova si chiude e
`settings.ini` torna com'era. Il test con mouse reale si interrompe da solo se il mouse viene mosso.
Se la finestra dell'utente è ridotta a icona, per creare quella di prova la si riapre un attimo senza
attivarla e poi torna ridotta a icona; se lo schermo non si può catturare (sessione Desktop remoto con il
client ridotto a icona) i controlli sui pixel si saltano con un avviso.

```powershell
.\build.ps1
.\tools\test\Run-Tests.ps1 -Restart -RealInput   # ricarica il Blocco note con la nuova build e testa
.\tools\test\Run-Tests.ps1                       # solo UI Automation, senza input reale
```

`-Restart` fa un backup della sessione in `%LOCALAPPDATA%\VTabsNotepad\backups`, chiude normalmente il
Blocco note (che ripristina da solo le schede) e lo riapre. Funziona solo con **una** finestra del
Blocco note aperta: chiudendo una finestra che non è l'ultima, il Blocco note ne scarta le schede,
quindi con più finestre il riavvio va fatto a mano (il launcher inietta la nuova versione da solo).
Il 2026-11-04, dopo giri di prova con la finestra di prova chiusa a fine prova, il Blocco note riaperto ha
mostrato due volte, insieme a quella dell'utente con tutte le sue schede, **una finestra in più con una
sola scheda vuota mai modificata** ("Senza titolo. Non modificato"), aperta nello stesso istante. Nei
riavvii dei giorni prima (registro della mod) non succedeva. Dopo un riavvio conviene contare finestre e
schede: quella finestra si può chiudere (non ha nulla dentro). Gli helper sono in `tools\test\TestKit.ps1`;
`VTabsNotepadUiaTool.exe` (compilato in `build\test`) apre il menu contestuale via UI Automation.

## Stato

**2026-11-06, barra di ricerca tagliata (Ctrl+F), segnalata dall'utente:** si vedeva solo il pezzo centrale. Mancavano a
sinistra la freccia di "Sostituisci" e l'inizio della casella, a destra le opzioni e la chiusura.
- **Causa.** Il Blocco note mette `NotepadXamlUI.FindReplaceDialog` in un'isola XAML a sé, sospesa in cima all'editor.
  La posiziona sul suo layout: centrata sull'editor senza colonna e larga circa 2/3 dell'editor (al 125%: 1001×95 px su
  1503). Ne impagina il contenuto per quella misura. `WindowLayout` la trattava come le barre di scorrimento: le spingeva
  il bordo sinistro oltre la colonna, stringendola da 1001 a 743 px, e la allungava verso l'alto come l'editor, da 95 a
  144 px. Il contenuto restava impaginato per l'altra misura e finiva tagliato ai due lati.
- **Correzione.** Le isole "sospese" (dentro l'editor, lontane almeno 8 DIP dai suoi bordi sinistro e destro) si spostano
  con l'editor senza cambiare misura (`IsFloating`, `PlaceFloating`): restano centrate come prima e salgono con la cima
  dell'editor.
- **Finestra stretta.** Se la barra non entra nell'editor ristretto dalla colonna (sotto i 790 px circa al 125%, col pannello
  espanso), si allinea al suo bordo destro e sporge sulla colonna. Il pannello va sotto di lei nell'ordine Z e torna in
  cima quando non serve più. Non si porta lei in cima: il Blocco note la tiene in fondo, sotto l'editor (il contenuto
  XAML è composto sopra comunque). Sopra l'editor, che ha `WS_CLIPSIBLINGS`, gli toglieva la sua zona, e nei margini
  trasparenti (l'ombra) si vedeva una fascia nera.
- **Prove.** Blocco nuovo in `Run-Tests.ps1` (7 controlli, eseguito da solo sulla finestra di prova): tutti i
  controlli della barra dentro la sua isola, letti con UI Automation (prima uscivano la freccia, la casella, "Cerca in
  alto", "Altre opzioni" ed "Esci da Trova e sostituisci"). Poi centro, "Sostituisci", pannello compresso, finestra stretta
  con il colore in cima all'editor (`Get-EditorTopColors`, con PrintWindow) e pannello di nuovo sopra l'editor dopo la
  chiusura.
- **Rilascio.** Blocco note riavviato con la mod nuova (13:13, 15 schede ripristinate).

**2026-11-05 sera, correzioni:**
- **Crash del Blocco note alla chiusura di una finestra**: `BackToTop` (Disable, timer di scomparsa, Show) e la
  visibilità del pannello di `VerticalTabs` chiamavano `SiteBridge().Show()/Hide()` su un'isola XAML la cui finestra
  era già distrutta (chiusura della finestra): accesso non valido in `IDesktopSiteBridge::Hide`, che `try/catch`
  non intercetta. Ora prima si controlla `IsWindow` sulla finestra dell'isola.
- **Log della mod**: ogni processo teneva aperto per sempre un handle su `mod.log`; quando un'istanza di un attimo
  del Blocco note (aprendo un file da fuori ne nasce una che passa il file a quella vera e termina, e anche lei
  carica la mod) ruotava il log, il Blocco note vero continuava a scrivere nel file rinominato, e le prove che
  leggono `mod.log` fallivano a caso ("Scheda vuota chiusa: il Blocco note non la ricorda", "TabId =
  identificativo della sessione"). Ora il file si apre a ogni riga e la dimensione si ricontrolla ogni 256 righe.
- Controllato che il Blocco note **non** trattenga le finestre chiuse con la diagnostica XAML (problema trovato in
  VTabsExplorer): 4 aperture e chiusure della finestra di prova, thread con finestre fermi a 3, memoria +3 MB.
- Suite `Run-Tests.ps1` 105/105 dopo il riavvio del Blocco note delle 19:01 (14 schede ripristinate).
- **Barra di scorrimento verticale che non arrivava in fondo** (segnalata dall'utente): con la riga del titolo
  nascosta l'isola della barra sale con l'editor (49 px al 125%), ma il Blocco note fissa l'altezza del suo
  `ScrollViewer` (`IFrameworkElement::put_Height` da `NotepadXamlUI.dll`, DIP interi) sull'isola non allungata:
  la barra restava ancorata in cima e in fondo mancavano 49 px. Un primo tentativo con un'animazione `HoldEnd`
  sopra il suo valore durava pochi secondi: in WinUI 3 una nuova `put_Height` vince sull'animazione conclusa, e il
  Blocco note la rifà a ogni ricalcolo del layout (ridimensionamento, cambio di finestra attiva; visto con la pila
  di chiamate). Ora `ScrollBarFit` scrive l'altezza del Blocco note + i px dell'editor salito e la riscrive dentro
  la stessa `put_Height` (callback sincrona su `Height`, nessun fotogramma con la barra corta); il valore scritto
  non è mai intero, così ogni `put_Height` del Blocco note si riconosce. Spegnendo la funzione torna il suo.
  Verificato nella finestra di prova: barra = isola (44..830 px) all'avvio, finestra più bassa/più stretta,
  ingrandita, ripristinata, con la barra orizzontale, cursore in fondo al documento che tocca il fondo; nella
  finestra dell'utente il Blocco note l'ha reimpostata più volte durante le prove e la mod l'ha sempre riallungata subito.
  Suite 107/107 con 2 controlli nuovi (barra fino in fondo, anche dopo un ridimensionamento).
- **Barra di scorrimento orizzontale che sforava fuori dalla finestra** (segnalata dall'utente subito dopo): stesso
  meccanismo sull'altro asse. L'isola della barra orizzontale si accorcia della colonna (258 px al 125%), ma il Blocco
  note dà al suo `ScrollViewer` la larghezza del layout senza colonna (`put_Width`): 928 DIP in un'isola di 721,6, e
  la parte destra della barra (freccia e fine corsa) restava fuori. In più il contenuto di ogni barra è un `Canvas`
  `ScrollContent` largo/alto vista + corsa del testo, e il Blocco note traduce la posizione della barra in quella del
  testo 1:1 su (contenuto − vista). Correggendo solo la vista: a testo tutto a destra il cursore orizzontale restava
  al 93,3%, la barra trascinata al 50% portava il testo al 53,5%, e la verticale trascinata in fondo lasciava il testo
  a 130 su 181 (le ultime righe non si raggiungevano trascinando: introdotto dalla correzione della verticale).
  Ora `ScrollBarFit` (un oggetto per asse) corregge vista **e** contenuto della stessa differenza dell'isola
  (`WindowLayout::onScrollBarsChange(raise, larghezza della colonna)`), quindi la corsa resta quella del Blocco note.
  Verificato nella finestra di prova: barra orizzontale espansa (mouse vero sopra) tutta dentro l'isola, cursore
  all'inizio e alla fine; trascinamenti a passi al 50% → testo al 50,0% su entrambi gli assi, fino in fondo → fine del
  testo; finestra ferma 10 s → 0 riscritture (nessun rimpallo), una riga → 2, 10 px di ridimensionamento → 18.
  Suite 112/112 con 5 controlli nuovi (verticale trascinata fino in fondo; orizzontale presente, in fondo a testo tutto
  a destra, larga quanto l'isola, a metà trascinata a metà).
- **Barre di scorrimento subito larghe sotto il mouse** (richiesta dell'utente: in Esplora file la barra reagisce
  subito, nel Blocco note no, anche senza mod): nuova funzione `ScrollBarHover`, voce nella tray `[ScrollBars]
  FastHover`. Il ritardo è `ScrollBarExpandBeginTime` = 0,4 s nel modello della `ScrollBar` di WinUI 3, trovato nei
  sorgenti (`ScrollBar_themeresources.xaml`, `ScrollBar_Partial.cpp`). Misure con il mouse vero sulla finestra di
  prova: senza la funzione la barra si allarga a 480 ms (completa a 650), con la funzione è già larga a 90 ms
  (completa a 200). L'interruttore funziona a caldo anche sulla stessa barra: accesa → 94 ms, spenta → 485 ms,
  riaccesa → 92 ms. Un primo ripristino non funzionava: teneva `weak_ref` alle singole animazioni, che WinUI
  rilascia; ora si ripercorre il modello della barra. Suite 115/115 (3 controlli nuovi dal registro della mod;
  con `-RealInput` altri 2 con il mouse vero: larga a 200 ms con la funzione, ancora sottile senza, riconosciuta
  dalle due frecce in cima e in fondo alla striscia).
- Subito dopo, su richiesta dell'utente ("lascia le modifiche, rimuovi solo l'opzione dal menu della tray così le
  rendiamo non disattivabili"): **tolta l'opzione**. Via la voce della tray, il campo `fastScrollBars` di `Settings` e
  la chiave `[ScrollBars] FastHover` (`Settings::Save` cancella la sezione; tolta subito anche da `settings.ini`).
  `ScrollBarHover` resta spenta solo per le finestre escluse dalla modalità sviluppo. La suite controlla ripristino e
  nuovo adattamento in quella fase: finestra di prova fuori dalla mod e poi di nuovo dentro. Con `-RealInput` resta il
  controllo "a 200 ms la barra è già larga". Suite 115/115 dopo il riavvio del Blocco note delle 22:46 (14 schede).
- **"Disattiva lo scorrimento fluido"** (richiesta dell'utente: opzione nella tray, attiva). Nuova funzione
  `WheelScroll` e voce `[SmoothScroll] Disabled=1`, scritta anche nelle impostazioni dell'utente.
  - Misure sulla finestra di prova, con scatti in coda e con la rotella vera: senza la funzione uno scatto fa ~10 passi
    in 0,3 s; con la funzione un solo salto della stessa distanza. Dalla cima sono 56 px: 20 px di margine più 2 righe
    da 18, uguale all'animazione; più in basso 36 px.
  - Ctrl+rotella resta lo zoom (altezza del documento 6280 → 7082 → 6280).
  - Le schede aperte dopo si agganciano da sole. Spegnendo l'opzione torna l'animazione, e torna istantaneo riaccendendo.
  - Suite 120/120 con 5 controlli nuovi. Nel Blocco note dell'utente dal riavvio delle 23:16 (14 schede).
- **"Lo scroll si blocca su alcune schede"** (segnalato dall'utente subito dopo, scheda "FASE 0"): né rotella né
  barra. Non era la rotella: anche con l'opzione spenta, quindi senza hook, l'animazione nativa non spostava
  nulla.
  - Con le schede verticali spente scorreva. La differenza erano i valori della barra della superficie: massimo
    verticale 1542 = orizzontale, contro i 906 veri.
  - È un difetto del Blocco note, riprodotto anche senza mod allungando la finestra: vedi "Corsa verticale
    fasulla". La vista più alta della mod lo fa emergere.
  - Correzione `ScrollRangeFix`; in più la rotella ha i ripieghi a pixel e nativo.
  - Verificato sulla scheda dell'utente (corretta all'avvio, barra fantasma sparita) e in una finestra di prova
    allungata e riaccorciata.
  - Suite 122/122 con 2 controlli nuovi. Nel Blocco note dell'utente dal riavvio delle 23:57 (13 schede).
- **Altri due difetti segnalati dall'utente** (2026-11-06): "in fondo al documento il testo continua a scorrere nel vuoto
  e poi torna indietro a scatti" e "il cursore della barra verticale cambia improvvisamente forma e grandezza mentre
  scorro".
  - I test di prima guardavano `SCROLLINFO`, che Windows tiene sempre dentro la corsa, quindi non potevano vederlo.
    Nuova sonda nel TestKit (`New-RichScrollProbe` / `Get-RichScrollPos` / `Set-RichScrollPos`): `EM_GETSCROLLPOS` e
    `EM_SETSCROLLPOS` con un buffer allocato nel processo del Blocco note.
  - Primo difetto, nostro. Il ripiego a pixel di `WheelScroll` (`EM_SETSCROLLPOS`) andava oltre la fine: in fondo
    `EM_LINESCROLL` non si muove, si ripiegava sui pixel e il testo finiva 36 px nel vuoto per ogni scatto. Lo stesso
    succedeva nei documenti che ci stanno in altezza. Senza mod, e con la rotella nativa, il testo si ferma a
    `nMax - nPage`. Ora il ripiego si ferma lì.
  - Secondo difetto, due cause. La prima è del Blocco note: la barra rimasta con la corsa vecchia, vedi la sezione
    apposita. `ScrollRangeFix` ha un timer proprio da 100 ms e rimanda `EN_UPDATE` al Blocco note quando le corse
    cambiano.
  - La seconda era nostra: l'isola di "Torna su" mostrata al primo scatto sotto la cima. Il movimento del mouse
    sintetico di Windows faceva passare la barra all'indicatore "da mouse", e dallo scatto dopo tornava quello di
    scorrimento. Ora l'isola resta parcheggiata e trasparente al mouse.
    - Trovato per gradi: fotogrammi classificati per indicatore, poi una diagnostica temporanea (eventi della barra
      e `IndicatorMode`: 1 e, 11 ms dopo, 2), poi spegnendo una funzione per volta. Non erano la rotella,
      ScrollBarFit, ScrollBarHover né ScrollRangeFix; spento "Torna su" il problema spariva.
    - Diagnostica e interruttori di prova tolti dal codice.
  - Sulle 13 schede dell'utente, per indice, rimesse poi alla loro posizione: barra uguale alla corsa dell'editor,
    in fondo 3 scatti senza movimento, nelle schede che ci stanno nessuno spostamento.
  - Suite con 5 controlli nuovi:
    - in fondo la rotella non va oltre;
    - la barra è giusta dopo l'apertura, aspettando che la corsa si fermi (il primo controllo passava anche senza
      correzione; ora fallisce su una finestra esclusa dalla mod, come deve);
    - il documento che ci sta non va nel vuoto;
    - a pulsante nascosto l'isola lascia il mouse al testo;
    - il pulsante compare senza mostrare finestre.
  - Suite 125/125 alle 01:27, al 125%, prima della modifica a "Torna su". Il giro dopo la modifica (02:00) è caduto
    mentre lo schermo passava a 3840×2160 al 225% e la finestra dell'utente veniva ridotta a icona: 120/125. I 5 falliti
    dipendono dalla geometria, non dalla mod:
    - finestra di prova molto più grande, e il documento "che ci sta" non poteva starci;
    - soglie in px del TestKit pensate per il 125%, per esempio l'isola orizzontale alta al massimo 30 px;
    - un popup sopra il punto di prova.
    Le verifiche mirate al 225% passano: in fondo fermo, documento che ci sta fermo, barra giusta dopo l'apertura,
    "Torna su" parcheggiato e cliccabile.
  - Da fare: rendere la suite indipendente dalla scala (soglie in DIP, misura fissa della finestra di prova). Nel TestKit
    ora `Get-ModLog -Since` legge anche `mod.old.log`, perché il log ruotato a metà prova faceva fallire "TabId";
    `Invoke-MenuCommand` aspetta il menu fino a 4 s. La seconda finestra di prova nello stesso processo PowerShell, con la
    finestra dell'utente ridotta a icona, non trova la voce "Nuova finestra": ogni prova va lanciata in un processo nuovo.

Verificato dal vivo (Blocco note 11.2607.14.0, Windows App SDK 2.5) con UI Automation, mouse e
tastiera reali e screenshot:

- selezione dal pannello e sincronizzazione inversa (Ctrl+Tab ecc.), cambi rapidi di scheda;
- "Nuova scheda", chiusura con X / clic centrale / `Canc` (stessa logica nativa, prompt compreso);
- X al passaggio del mouse; pallino al posto della X sulle schede modificate, anche su quella attiva (1.2.2,
  confrontato con le schede native senza mod: attiva modificata = pallino, X con il mouse sopra; attiva salvata = X);
- larghezza, finestra piccola e massimizzata. Il lato destro e la ricerca nelle schede, verificati anche
  loro, sono stati tolti il 2026-11-04 e il 2026-11-05;
- disattivazione/riattivazione con ripristino completo del layout originale;
- pagina Impostazioni del Blocco note (il pannello si fa da parte), tema chiaro/scuro;
- barra del titolo trascinabile anche dove c'erano le schede;
- colonna in cima alla finestra come Chrome (2026-11-04, `Run-Tests.ps1 -RealInput` 119 su 119): dalla
  cima al fondo dell'editor, barra dei menu dal suo bordo, pulsanti del pannello nella riga di quelli della
  finestra; con il mouse vero la fascia del titolo trascina la finestra e il doppio clic la ingrandisce
  (colonna sempre in cima) e la ripristina;
- riga del titolo nascosta (2026-11-04, subito dopo): barra dei menu nella riga del titolo fino ai pulsanti
  della finestra, editor e superficie del testo allungati verso l'alto. `Run-Tests.ps1` 99 su 99 senza input
  reale; con `-RealInput` 85 su 85 prima che la guardia fermasse la prova (mouse mosso dall'utente, al PC in
  quel momento), compresi tutti i controlli nuovi e trascinamento e doppio clic sulla fascia. Le prove col
  mouse rimaste (riordino, divisore) non sono state ripetute con questa versione: erano
  passate con quella di prima (la parte toccata è solo la posizione di barra dei menu ed editor);
- pannello a destra tolto e fascia scura in cima all'editor corretta (2026-11-04, 09:16): nel menu della tray la
  voce non c'è più, la chiave `Position` sparisce da `settings.ini` al primo salvataggio (verificato), icona di
  comprimi/espandi sempre quella per il lato sinistro. I tre controlli nuovi sulla fascia scura fallivano con la
  versione precedente (fascia `#202020` su `#474747`) e passano con la correzione: `Run-Tests.ps1` 102 su 102
  senza input reale; CPU del Blocco note a riposo 16 ms in 8 s (nessun ciclo di ridisegno);
- fascia chiara in cima all'editor salito, segnalata dall'utente, corretta (2026-11-04, 09:36): era la zona
  vetro del Blocco note. Con la correzione l'editor è `#272727` dalla cima, con la finestra normale, ingrandita e
  ripristinata. Dal registro, ingrandendo il Blocco note chiede 91 px e ripristinando 93, e la mod li riduce a 44.
  Spegnendo la colonna torna a 93. `Run-Tests.ps1` 103 su 103 senza input reale. Al primo giro "Torna su: in cima
  il pulsante sparisce" era fallito una volta: è un controllo già instabile, con codice non toccato.
  CPU a riposo 16 ms in 8 s;
- zona vuota della barra dei menu trascinabile, segnalata dall'utente (2026-11-04, 09:57): buco nella regione
  dell'isola tra i comandi. Al 125% va da x=280 a 595 nell'isola, e fino a 819 con la finestra ingrandita o il
  pannello compresso. La linea in fondo alla barra resta continua. `Run-Tests.ps1` 107 su 107 senza input reale;
  il trascinamento col mouse vero non è stato provato, ma usa lo stesso meccanismo della fascia in cima alla
  colonna, già provato col mouse vero;
- ricerca nelle schede tolta (2026-11-05, richiesta dell'utente):
  - via la lente, la casella, il filtro dell'elenco e il blocco del riordino con il filtro attivo;
  - nella fascia del titolo resta solo comprimi/espandi;
  - nel menu della tray non c'è più la voce; `Settings::Save` cancella la vecchia chiave `SearchBox`, verificato;
  - test: `Run-Tests.ps1` 103 su 103 senza input reale (al primo giro 102 su 103 per il solito "Torna su");
- pulsanti della finestra alti quanto la barra dei menu (2026-11-05, richiesta dell'utente insieme a VTabsExplorer: "come
  fanno chrome e windows terminal"):
  - nella finestra di prova al 125%: 58 px l'uno, alti 44 (dalla cima della finestra al fondo della barra dei menu),
    glifi centrati come l'icona delle impostazioni; pagina Impostazioni: tornano quelli di DWM, e i nostri al ritorno
    nell'editor;
  - mouse vero sulla finestra di prova: chiudi rosso al passaggio, ingrandisci (pulsanti dalla cima dello schermo) e
    ripristina, menu di sistema col tasto destro, riduci a icona e ripristino, chiudi (finestra di prova chiusa);
  - `Run-Tests.ps1` 105 su 105 senza input reale (con i controlli nuovi); al secondo giro 94 su 94, con i controlli sui
    colori delle righe saltati perché la finestra di prova non era in primo piano;
  - nel Blocco note dell'utente dal riavvio delle 14:38 (14 schede; chiusa la solita finestra vuota in più).
- `Run-Tests.ps1 -RealInput`: rimossa la prova del riordino con il filtro attivo;
- seconda finestra nello stesso processo, chiusura delle finestre senza perdite né crash;
- avvio normale del Blocco note con il launcher nella tray: pannello in circa 0,7 s;
- nessun costo di CPU a riposo (identico a funzione spenta);
- riordino con trascinamento in su e in giù, anche con 25 schede e scorrimento automatico; ordine
  salvato nella sessione del Blocco note e usato dai suoi comandi;
- menu contestuale nativo con mouse e da UI Automation: compare sotto il cursore e tutte e quattro
  le voci agiscono sulla scheda giusta; la striscia nativa torna fuori vista alla chiusura;
- divisore: cursore ↔, trascinamento, doppio clic, larghezza salvata e applicata alle altre finestre;
- sfondo del pannello identico alla barra del titolo (misurato: `#202020` entrambi, tema scuro con
  trasparenza di Windows spenta), anche da compresso; con il pannello attivo anche la barra dei menu
  è `#202020` (prima `#2C2C2C`, come l'editor); senza pannello torna `#2C2C2C`, unita alla scheda
  orizzontale attiva come nell'originale;
- pannello compresso (56 DIP) ed espanso: l'editor si allarga e si restringe, stato salvato, elenco
  completo, riordino con trascinamento anche da compresso;
- colori: sottomenu "Colore scheda" con Nessuno + 9 colori; riga intera in tinta tenue (misurata
  sullo schermo: `#3F302E` per il rosso sul `#202020`, `#2A2A2A` il velo delle righe senza colore) e
  icona nel colore pieno, anche da compresso; "Nessuno" riporta la riga com'era; salvataggio in
  `tab-colors.ini`; il `TabId` coincide con l'identificativo della sessione, quindi i colori
  sopravvivono al riavvio; fuori dal pannello il menu nativo resta quello originale;
- scheda attiva: tinta più decisa (`#583D3A` per il rosso), bordino (`#6E5754`), stanghetta nel colore
  della scheda (`#DA7D75`) o, senza colore, grigio neutro (`#A4A4A4`); titolo più spesso; lasciandola,
  la riga torna come prima;
- titoli identici alle schede orizzontali: carattere, dimensione e peso (400 normale, 600 attiva)
  confrontati via UI Automation; colore misurato sullo schermo con le schede orizzontali visibili
  (normale `#CCCCCC` contro `#CFCFCF` del pannello, che ha il fondo appena più chiaro; attiva `#FFFFFF`
  in entrambi); al passaggio del mouse il Blocco note non cambia il colore del titolo (verificato col
  mouse reale) e nemmeno il pannello;
- "+" subito sotto l'ultima scheda, centrato e senza scritta; con molte schede l'elenco scorre e il
  "+" resta in fondo; "comprimi e subito espandi" non rimbalza;
- icone solo a pannello compresso: da espanso nessuna iniziale (il colore resta nella tinta della
  riga), da compresso le iniziali compaiono con il colore della scheda;
- editor: la superficie di testo è larga quanto il contenitore (a pannello espanso, compresso, dopo
  un cambio di scheda) e torna a tutta larghezza senza pannello; con l'a capo automatico il testo va a
  capo dentro l'area visibile, prima della barra di scorrimento verticale; la rotella orizzontale non
  fa scorrere l'elenco delle schede (controlli automatici in `tools\test\Run-Tests.ps1`);
- "riapri la scheda chiusa" con Ctrl+Maiusc+T (2026-11-02, Windows 26H2 build 26300): **89 controlli
  su 89**, compresi gli 11 nuovi. La scheda chiusa torna al posto esatto che aveva (verificato: comparsa
  al posto 6 e rimessa al 3), due schede tornano a ritroso, premendo subito dopo la chiusura torna
  quella scheda, una scheda mai salvata viene vista e saltata, e la combinazione risulta riservata al
  Blocco note solo mentre la sua finestra è attiva.
  La pressione vera di Ctrl+Maiusc+T è stata provata a parte (la suite non preme tasti), con il fuoco
  nell'editor: la scheda è tornata al suo posto. Provati anche due casi limite: un file con percorso di
  233 caratteri (spazi e accenti) e una scheda **modificata** chiusa con "Non salvare" — tornano
  entrambe, quindi la descrizione della scheda resta il percorso nudo anche quando la scheda è segnata
  come modificata.
  Dopo una revisione del codice sono stati corretti: una scrittura su memoria già liberata a ogni
  chiusura di finestra (il nostro `WM_NCDESTROY` aggiornava lo stato dopo aver proseguito la catena dei
  subclass, e in quella catena la mod libera le feature della finestra, cioè distrugge l'oggetto);
  la pressione entro mezzo secondo dalla chiusura, che riapriva la scheda sbagliata perché la chiusura
  non era ancora confermata (ora al comando la conferma è immediata); due pressioni ravvicinate, che
  lasciavano la prima scheda in fondo invece che al suo posto (ora le riaperture in volo possono essere
  più d'una); la scorciatoia chiesta a Windows anche quando l'aggancio alla finestra era fallito, cioè
  togliendola agli altri programmi senza poterla usare. Per sicurezza la combinazione viene restituita
  anche se la finestra viene distrutta mentre la teneva (la registrazione è legata al thread, non alla
  finestra). Verificato sul log del processo: 87 volte presa, 87 restituita, nessuna registrazione
  fallita — quando il fuoco va altrove la combinazione torna sempre libera;
- pulsante "Torna su" (2026-10-29): 77 controlli su 77, compresi i 13 del pulsante (comparsa,
  posizione, clic fino alla riga 1, focus, cambio di scheda, pannello compresso/espanso, opzione) e
  quelli sui pixel dello schermo (60 su 60 con i soli controlli UI Automation quando lo schermo non si
  può catturare). Aspetto verificato su una cattura della finestra di prova: cerchio grigio con la
  freccia, bordi lisci, testo visibile tutt'intorno; si alza da solo sopra la barra di scorrimento
  orizzontale.

Limiti noti: dal pannello non si può staccare una scheda in una nuova finestra trascinandola fuori;
all'avvio del Blocco note la barra orizzontale resta visibile per una frazione di secondo. Su
Windows 11 l'icona della tray nuova finisce tra le icone nascoste ("^"): si può trascinare sulla
barra delle applicazioni.

Icona della tray: un blocco note monocromo a contorno, con tre anelli in alto e le righe, che richiama il
Blocco note senza essere la sua icona.
- È bianca sulla barra delle applicazioni scura e nera su quella chiara, come le icone di sistema.
- È disegnata con GDI+ alla misura vera, a ogni cambio di scala e di tema. La griglia di 16 unità è
  agganciata ai pixel, e anelli e righe hanno un passo intero, quindi restano nitidi ed equidistanti
  anche a 20 px (schermo al 125%).
- Se GDI+ non parte, usa il glifo "DockLeft" di Segoe Fluent Icons.

Possibili sviluppi: staccare una scheda in una nuova finestra trascinandola fuori dal pannello,
titolo del documento nella barra del titolo, colore delle schede anche nella barra orizzontale,
gruppi di schede.

## Note tecniche emerse dai test

- Il Blocco note crea **una finestra editor (`NotepadTextBox` + `RichEditD2DPT`) per ogni documento
  aperto, ciascuna con il proprio thread**, e mostra solo quella della scheda attiva.
- L'editor disegna nella superficie della finestra principale: spostandolo, l'area lasciata libera
  conserva un'immagine "fantasma" del testo. Per questo il pannello ha uno sfondo opaco.
- La radice dell'isola della barra schede ha dimensioni esplicite (non si può "allargare"): da qui la
  scelta di un'isola XAML propria per il pannello.
- Ogni avvio di `notepad.exe` crea un breve processo che inoltra la richiesta all'istanza già aperta.
  Il launcher inietta anche in quello, che poi termina senza conseguenze. È il meccanismo con cui
  "riapri la scheda chiusa" rimette il file nella finestra attiva (in circa 0,4 s), ma durante quei
  pochi istanti esistono **due processi `Notepad`**: gli helper dei test che cercano per PID (voci di
  menu, log della mod) devono prendere quello con le finestre, altrimenti guardano il processo sbagliato
  (`Get-NpProcess` in `TestKit.ps1`).
- `RegisterHotKey` lega la registrazione al **thread**, non alla finestra indicata: la mod la restituisce
  anche in `WM_NCDESTROY` (la finestra esiste ancora lì), così non resta occupata se il thread della
  finestra sopravvive. Attenzione nel misurare: prima di dire che una combinazione è "rimasta occupata"
  va guardato **quale** finestra è in primo piano — se è una del programma che la usa, tenerla è
  corretto.
- Il percorso del file di una scheda si legge dal `Tooltip` del suo `TabItemViewModel` (per una scheda
  mai salvata è "Senza titolo", 12 caratteri: non essendo un file esistente, non viene ricordata).
  Chiudendo la scheda attiva diventa attiva quella a **destra**, e l'editor della scheda chiusa viene
  distrutto: non c'è niente da cui "recuperare" un documento chiuso, che è anche il motivo per cui
  "riapri la scheda chiusa" vale solo per i file salvati.
- Il drag & drop OLE di WinUI non arriva a un'isola `DesktopWindowXamlSource` aggiunta da noi
  (nessuna isola ha un `IDropTarget` proprio: WinUI instrada tramite un `DragDropManager` per isola).
  Per questo il riordino è gestito con gli eventi puntatore: cattura sull'elenco, riga spostata dal
  vivo, `ReorderTab` al rilascio.
- Le animazioni predefinite della `ListView` (`AddDeleteThemeTransition`) tengono una riga rimossa
  agganciata all'elenco mentre sfuma: con righe che fanno da contenitore di sé stesse, reinserirla
  subito fallisce e la `ListView` si disallinea dalla raccolta. Resta solo `RepositionThemeTransition`
  (e, per sicurezza, una ricostruzione completa dell'elenco se una modifica venisse rifiutata).
- `TabsBar.ReorderTab(da, a)` toglie e reinserisce lo stesso `TabItemViewModel` (indice finale), così
  la riga del pannello viene riusata.
- Il menu contestuale si apre con `FlyoutBase.ShowAt` sul `TabViewItem` nativo e la posizione del clic;
  il bersaglio deve stare nella stessa isola del menu, da qui lo spostamento temporaneo della striscia.
- Il cursore ↔ del divisore si imposta con `ProtectedCursor`, raggiungibile su qualunque elemento
  tramite l'interfaccia `IUIElementProtected`.
- Gli sfondi delle righe della `ListView` (passaggio del mouse, selezione) sono rientrati di 4 DIP ai
  lati e 2 DIP sopra e sotto rispetto al contenuto: il fondo delle righe e il "+" usano lo stesso
  rientro per combaciare. Fondo e stanghetta di selezione predefiniti sono spenti con risorse locali
  della `ListView` (`ListViewItemBackgroundSelected*`, `ListViewItemSelectionIndicator*Brush`): la
  scheda attiva la disegna la riga, nel colore della scheda; il passaggio del mouse resta quello di
  sistema.
- Il testo delle righe di una `ListView` eredita 14 (`ControlContentThemeFontSize`), quello delle
  schede del `TabView` è 12 (`TabViewItemHeaderFontSize`): il pannello legge carattere, dimensione,
  peso e colore dal titolo di schede native già realizzate (una nello stato visivo `Normal`, una in
  `Selected`) e li applica ai suoi titoli; li rilegge ogni 2 secondi e al cambio di tema. Il colore va
  cercato risalendo dal testo agli antenati: il `TextBlock` creato dal `ContentPresenter` lo eredita e
  il suo `Foreground` può risultare vuoto. I `Setter` degli stati visivi dei modelli di sistema non si
  lasciano leggere (E_FAIL): il colore "al passaggio del mouse" si ottiene portando per un istante la
  scheda nativa (invisibile) nello stato `PointerOver` con `VisualStateManager.GoToState`, leggendolo e
  ripristinando subito lo stato precedente, nello stesso passaggio del thread UI.
- I test dal vivo, se l'utente ha spento le schede verticali, le accendono solo nella finestra di prova
  (allowlist `dev-windows.txt`) e alla fine rimettono prima `Enabled=0` e poi tolgono l'allowlist.
- Lo sfondo della barra dei menu non viene sovrascritto: un `Storyboard` lo tiene trasparente (le
  animazioni hanno la precedenza sui valori locali) e, fermandolo, torna il valore del Blocco note con
  i suoi riferimenti al tema. Ogni finestra trova la propria barra tramite la `XamlRoot` della sua
  isola (`ContentIslandEnvironment.AppWindowId`), come per la `TabsBar`.
- Il contenitore dell'editor (`NotepadTextBox`) non adatta la superficie di testo che contiene
  (`RichEditD2DPT`): restringendo solo il contenitore, la superficie restava larga come prima e il
  testo andava a capo oltre il bordo visibile, sotto la barra di scorrimento verticale (e senza a capo
  automatico le righe lunghe non si potevano scorrere fino in fondo). `WindowLayout` restringe della
  stessa quantità anche le finestre interne che sporgerebbero, e le riporta com'erano quando il
  pannello si chiude. Nel Blocco note l'a capo automatico vale per scheda.
- Lo `ScrollViewer` di WinUI, quando non può scorrere in orizzontale, converte la rotella orizzontale
  in scorrimento verticale; lo scorrimento a due dita del touchpad arriva proprio così. Per questo
  l'elenco ferma gli eventi di rotella orizzontale sul contenitore delle righe (`ItemsPresenter`),
  prima che raggiungano lo `ScrollViewer`: scorre solo in verticale.
- Le modifiche a `settings.ini` arrivano a raffiche: la lettura del thread di controllo può essere già
  superata da una scelta fatta nel frattempo (es. comprimi e subito espandi). Per questo ogni finestra,
  quando applica le impostazioni, rilegge il file: vale sempre l'ultima scelta salvata.
- "Torna su": l'animazione di sistema (`ScrollViewer.ChangeView` animato) sulla barra di scorrimento del
  Blocco note si ferma a metà (l'editor risaliva solo fino alla riga 20 di 160), contrastata dalla
  sincronizzazione barra-editor; con salti senza animazione l'editor la segue sempre. Il pulsante quindi
  anima da sé: un salto ogni 15 ms per 260 ms, rallentando verso la fine.
- Su un documento che non scorre il Blocco note nasconde l'isola della barra di scorrimento senza
  aggiornarne lo `ScrollViewer` (che resta alla posizione del documento di prima): conta anche
  `XamlRoot.IsHostVisible`, con l'evento `XamlRoot.Changed`.
- Barre di scorrimento dell'editor (`NotepadXamlUI.ScrollBar`, ciascuna nella sua isola): il Blocco note fissa con
  valori locali in DIP interi la misura dello `ScrollViewer` (vista) e del suo contenuto, un `Canvas` `ScrollContent`
  grande vista + corsa del testo, sull'asse della barra; li reimposta a ogni ricalcolo del layout e a ogni
  scorrimento, e traduce la posizione della barra in quella del testo 1:1 su (contenuto − vista). In WinUI 3 un
  valore locale nuovo vince anche su un'animazione `HoldEnd` già conclusa (stato `Filling`, che resta tale ma non ha
  più effetto): per tenere valori diversi dai suoi bisogna riscriverli nella callback di
  `RegisterPropertyChangedCallback`, che arriva sincrona dentro la sua `put_`.
- `weak_ref` a oggetti XAML senza stato proprio (per esempio le animazioni dentro gli storyboard di un modello)
  possono scadere mentre l'oggetto vive ancora: WinUI rilascia l'involucro WinRT (peer) quando nessuno lo tiene e
  ne crea uno nuovo alla richiesta successiva. Per ritrovarli si riparte da un elemento tenuto vivo (una
  `ScrollBar`, tenuta dal suo `ScrollViewer`) e si ripercorre l'albero.
- Barra verticale dell'editor: la prima volta che si mostra dopo l'apertura di un documento, per un istante il suo
  cursore può essere molto più lungo del dovuto (catture a 60-270 ms); poi si sistema. Nelle misure si guardano le
  frecce, non la quantità di pixel.
- La barra XAML verticale del Blocco note segue i valori della barra di scorrimento Win32 della superficie di testo:
  correggendoli con `SetScrollInfo` (anche da un altro thread o processo) si aggiorna o sparisce. Con `SendMessageTimeout`
  verso un editor (thread proprio) i puntatori passati restano validi, perché si è nello stesso processo. Se il tempo
  scade, però, l'editor può ancora scriverci più tardi: i buffer stanno nello heap e in quel caso non si liberano.
- Ogni scheda del Blocco note ha il suo editor (`NotepadTextBox` > `RichEditD2DPT`) su un thread proprio, diverso da
  quello della finestra (anche le schede nascoste, se già aperte). Per intercettare i messaggi dell'editor serve un hook
  per thread (`WH_GETMESSAGE`, stesso processo: `hMod` nullo). Un hook `WH_GETMESSAGE` vede solo i messaggi in coda:
  nelle prove la rotella va mandata con `PostMessage`, perché `SendMessage` lo salta (e mostra l'animazione dell'editor).
- Trascinamenti simulati con UI Automation (`ScrollPattern.SetScrollPercent`) sulle barre del Blocco note: un salto
  unico lungo si ferma a metà (verticale al 100% in un documento di 400 righe: testo al 18%, barra tornata al 50%), e
  un salto fatto subito dopo uno scorrimento del testo viene ignorato. A piccoli passi (20 da 60 ms,
  `Invoke-ScrollDrag`) il testo segue sempre, come con la mano.
  PrintWindow mostra una striscia nera di 4 px a destra della barra, nei px dell'editor salito: sullo schermo
  vero non c'è (controllato con `CopyFromScreen`), è un artefatto della cattura.
- Un'isola XAML figlia della finestra del Blocco note, resa `WS_EX_LAYERED` (senza superficie GDI, come
  l'isola della barra di scorrimento), è trasparente dove il suo contenuto non disegna: sotto si vede il
  testo vivo dell'editor. Il pannello delle schede invece non è layered e deve restare opaco.
- Test con input sintetico: `SetCursorPos` non genera gli eventi puntatore di WinUI (`WM_POINTER*`),
  servono movimenti `mouse_event` assoluti; l'input iniettato può essere elaborato in ritardo, quindi si
  aspetta che il cursore arrivi invece di correggerlo. UI Automation vede solo le schede native
  realizzate dalla striscia (con molte schede l'ordine completo si legge dalla sessione salvata).

## Disattivare o rimuovere

Dal menu della tray: "Schede verticali" (on/off) oppure "Esci".

**"Esci" spegne la mod** (dalla 1.2.3; prima restava accesa nelle finestre aperte finché il Blocco note non si
chiudeva, segnalato dall'utente il 2026-11-07). Alla fine di wWinMain il launcher scrive `tray-exited.txt` nella
cartella dei dati; `ModCore::ReloadSettings` lo vede (la mod osserva la cartella) e mette `trayExited` con tutte le
funzioni spente, solo in memoria: `Settings::Load` e `Save` non lo toccano, perché la colonna riscrive settings.ini
intero quando cambiano larghezza o compressione. `VerticalTabs::Apply` (che rilegge il file da sé) prende
`trayExited` dall'avviso; `ScrollBarHover` e `ScrollRangeFix`, sempre attive, si spengono con lui; `ReopenTab` si
spegne senza cancellare la cronologia (la cancella solo spenta dal menu). Anche senza settings.ini vale spenta
(dati eliminati disinstallando: i valori predefiniti sono accesi). All'avvio il launcher cancella il file e tutto
torna. Solo "Esci" e `--exit` (anche gli installer, che non mostrano più "restano fino alla chiusura") lo
scrivono; `--exit --keep-mods` no. Prova: scratchpad `np-exit-test.ps1` (solo conteggi, nessun contenuto). Per rimuovere tutto: chiudere la
tray, togliere "Avvia con Windows" se attivato, cancellare `%LOCALAPPDATA%\VTabsNotepad` e la cartella
del progetto. Il Blocco note non viene mai modificato su disco.

## Lingua dei testi (dalla 1.1.0)

I testi per l'utente (menu della tray, menu "Colore scheda" e nomi dei colori, suggerimenti, nomi per
l'accessibilità) esistono in italiano e in inglese. `UiItalian()` in `Shared.h` legge `GetUserDefaultUILanguage()`
una volta per processo: italiano se la lingua di visualizzazione di Windows è l'italiano, altrimenti inglese
(cambiarla richiede di disconnettersi). `Tr(inglese, italiano)` sceglie il testo. Il markup XAML resta scritto in
italiano: in inglese i valori degli attributi per l'utente si sostituiscono prima di `XamlReader::Load`. I log
restano in italiano. L'installazione sceglie la lingua allo stesso modo (Inno Setup, `ShowLanguageDialog=no`). Il
kit delle prove cerca gli elementi con `UiName 'inglese' 'italiano'`.

Non affiliato a Microsoft.
