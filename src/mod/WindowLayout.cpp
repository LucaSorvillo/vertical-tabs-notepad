#include "pch.h"
#include "WindowLayout.h"

#include "Glass.h"
#include "Interop.h"
#include "Log.h"
#include "PositionPin.h"

namespace npx
{
    namespace
    {
        constexpr UINT_PTR kSubclassId = 0x4E505832; // 'NPX2'
        constexpr int kBandTolerance = 2;            // px di tolleranza per "dentro la fascia dell'editor"
        constexpr int kMinEditorWidthDip = 200;      // l'editor non scende mai sotto questa larghezza
        constexpr ULONGLONG kNoEditorGraceMs = 400;  // tolleranza "nessun editor visibile" (cambio scheda)
        constexpr int kOffscreenMargin = 4000;       // di quanto si sposta a sinistra la striscia nativa
        // Riga del titolo nascosta: px lasciati in cima sopra la barra dei menu (il bordo superiore, che ridimensiona) e
        // spazio a destra della barra dei menu per i pulsanti della finestra (3 da 46 DIP) più un tratto da trascinare.
        constexpr int kTitleTopInsetPx = 3;
        constexpr int kCaptionReserveDip = 3 * static_cast<int>(CaptionButtons::kButtonWidthDip) + 16;

        // Hook WinEvent attivi -> layout proprietario (le callback arrivano sul thread che li ha installati).
        std::mutex g_hooksLock;
        std::unordered_map<HWINEVENTHOOK, WindowLayout*> g_hooks;

        bool IsEmpty(RECT const& r)
        {
            return r.right <= r.left || r.bottom <= r.top;
        }

        bool Same(RECT const& a, RECT const& b)
        {
            return EqualRect(&a, &b) != FALSE;
        }

        // Figlio diretto di 'main' che contiene 'descendant' (o 'descendant' stesso).
        HWND DirectChildOf(HWND main, HWND descendant)
        {
            for (HWND current = descendant; current;)
            {
                const HWND parent = GetAncestor(current, GA_PARENT);
                if (parent == main)
                {
                    return current;
                }
                current = parent;
            }
            return nullptr;
        }

        // Rettangolo nelle coordinate client del genitore (quelle che usa SetWindowPos per le figlie).
        RECT RectInParent(HWND hwnd)
        {
            return interop::RectInClientOf(hwnd, GetAncestor(hwnd, GA_PARENT));
        }

        std::vector<HWND> DirectChildren(HWND main)
        {
            std::vector<HWND> result;
            for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                result.push_back(child);
            }
            return result;
        }

        // Toglie la colonna del pannello (a sinistra) dal rettangolo naturale di una finestra figlia: ciò che inizia
        // dentro la colonna viene spinto a destra di paneWidth (traslato se vi sta tutto dentro, altrimenti se ne
        // accorcia il bordo sinistro). Idempotente: applicata a un rettangolo già fuori dalla colonna lo lascia invariato.
        RECT Transform(RECT r, int paneWidth)
        {
            if (r.left < paneWidth)
            {
                if (r.right <= paneWidth)
                {
                    OffsetRect(&r, paneWidth, 0);
                }
                else
                {
                    r.left = std::min<LONG>(r.left + paneWidth, r.right);
                }
            }
            return r;
        }

        // Isola sospesa sopra l'editor (la barra di ricerca di Ctrl+F, centrata in cima all'editor): sta dentro l'editor
        // senza toccarne i bordi sinistro e destro (la barra di scorrimento verticale tocca il destro, l'orizzontale il
        // sinistro). Rettangoli naturali, coordinate client; gap = distanza minima dai bordi.
        bool IsFloating(RECT const& r, RECT const& editor, int gap)
        {
            return r.left >= editor.left + gap && r.right <= editor.right - gap && r.top >= editor.top - gap && r.bottom <= editor.bottom + gap;
        }

        // Un'isola sospesa si sposta con l'editor senza cambiare misura: il suo contenuto XAML il Blocco note lo impagina per
        // quella misura (ristretta dal bordo sinistro e allungata verso l'alto come l'editor, la barra di ricerca restava
        // tagliata ai due lati). Resta alla stessa distanza dal centro dell'editor (la barra di ricerca centrata) e dentro
        // l'editor; se è vicina alla cima (fino a anchorPx) sale con lei. Più larga dell'editor (finestra stretta: la barra
        // di ricerca è larga circa 2/3 dell'editor del Blocco note, che non sa della colonna): a gap px dal bordo destro
        // dell'editor e, se serve, sopra la colonna (chi chiama mette il pannello sotto di lei), mai fuori dalla finestra a
        // sinistra.
        RECT PlaceFloating(RECT r, RECT const& editorNatural, RECT const& editorWanted, int raise, int anchorPx, int gap)
        {
            OffsetRect(&r, ((editorWanted.left + editorWanted.right) - (editorNatural.left + editorNatural.right)) / 2, 0);
            if (r.right > editorWanted.right)
            {
                OffsetRect(&r, editorWanted.right - r.right, 0);
            }
            if (r.left < editorWanted.left)
            {
                OffsetRect(&r, std::max<LONG>(0, editorWanted.right - gap - (r.right - r.left)) - r.left, 0);
            }
            if (raise > 0 && r.top - editorNatural.top <= anchorPx)
            {
                OffsetRect(&r, 0, -raise);
            }
            return r;
        }
    }

    WindowLayout::~WindowLayout()
    {
        Detach();
    }

    void WindowLayout::Attach(HWND main)
    {
        m_main = main;
        // Subclass sempre presente (anche a feature spenta, senza modificare nulla): ci serve per
        // accorgerci in modo affidabile della chiusura della finestra (WM_NCDESTROY).
        if (!m_subclassed && m_main && GetWindowThreadProcessId(m_main, nullptr) == GetCurrentThreadId())
        {
            m_subclassed = SetWindowSubclass(m_main, &SubclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
        }
    }

    void WindowLayout::SetManagedWindows(HWND nativeTabs, HWND panel)
    {
        m_nativeTabs = nativeTabs;
        m_panel = panel;
    }

    void WindowLayout::Detach()
    {
        RemoveHooks();
        // Pulsanti della finestra: di nuovo quelli di DWM se la finestra resta (si stacca anche chiudendola: allora no).
        m_buttons.Remove(m_main && IsWindow(m_main) && IsWindowVisible(m_main));
        if (m_main)
        {
            glass::Forget(m_main); // (senza ripristino: la finestra si sta chiudendo)
        }
        if (m_regionHwnd && IsWindow(m_regionHwnd))
        {
            SetWindowRgn(m_regionHwnd, nullptr, TRUE);
        }
        m_regionHwnd = nullptr;
        m_regionHoles.clear();
        m_menuBarHwnd = nullptr;
        if (m_subclassed)
        {
            if (IsWindow(m_main))
            {
                RemoveWindowSubclass(m_main, &SubclassProc, kSubclassId);
            }
            m_subclassed = false;
        }
        for (auto const& [hwnd, tracked] : m_tracked)
        {
            pin::Clear(hwnd);
        }
        m_tracked.clear();
        m_active = false;
    }

    void WindowLayout::SetActive(bool active, Settings const& settings)
    {
        m_settings = settings;
        if (!m_main || !IsWindow(m_main))
        {
            return;
        }

        if (active)
        {
            if (!m_active)
            {
                m_active = true;
                InstallHooks();
            }
            Check();
        }
        else if (m_active)
        {
            m_active = false;
            RemoveHooks();
            RestoreChildren();
            glass::SetTopLimit(m_main, std::nullopt, m_glassFallbackTop);
            SetPanelShown(false);
            RequestNativeRelayout();
            NPX_LOG(L"WindowLayout: layout originale ripristinato.");
        }
    }

    void WindowLayout::InstallHooks()
    {
        // Solo eventi del nostro processo, consegnati in modo asincrono a questo thread. Due hook
        // mirati invece di un intervallo ampio: si evitano focus/selezione/stato dell'editor.
        const auto install = [this](HWINEVENTHOOK& hook, DWORD first, DWORD last) {
            if (hook)
            {
                return;
            }
            hook = SetWinEventHook(first, last, nullptr, &WinEventProc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
            if (hook)
            {
                std::scoped_lock lock(g_hooksLock);
                g_hooks[hook] = this;
            }
        };
        install(m_hookShowHide, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE);
        install(m_hookLocation, EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
        NPX_LOG(L"WindowLayout: hook WinEvent %s, subclass %s.", (m_hookShowHide && m_hookLocation) ? L"OK" : L"KO",
                m_subclassed ? L"OK" : L"no (finestra su altro thread)");
    }

    void WindowLayout::RemoveHooks()
    {
        for (HWINEVENTHOOK* hook : { &m_hookShowHide, &m_hookLocation })
        {
            if (*hook)
            {
                {
                    std::scoped_lock lock(g_hooksLock);
                    g_hooks.erase(*hook);
                }
                UnhookWinEvent(*hook);
                *hook = nullptr;
            }
        }
    }

    void CALLBACK WindowLayout::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
    {
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
        {
            return; // cursore, caret, oggetti MSAA interni: non ci interessano
        }
        if (event != EVENT_OBJECT_LOCATIONCHANGE && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE)
        {
            return;
        }

        WindowLayout* self = nullptr;
        {
            std::scoped_lock lock(g_hooksLock);
            if (const auto it = g_hooks.find(hook); it != g_hooks.end())
            {
                self = it->second;
            }
        }
        if (!self || !self->m_active || hwnd == self->m_panel || interop::IsOverlay(hwnd))
        {
            return;
        }
        // Finestra principale, sue figlie dirette e finestre interne degli editor (nipoti).
        const HWND parent = GetAncestor(hwnd, GA_PARENT);
        if (hwnd == self->m_main || parent == self->m_main || (parent && GetAncestor(parent, GA_PARENT) == self->m_main))
        {
            self->Check();
        }
    }

    LRESULT CALLBACK WindowLayout::SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
    {
        auto* self = reinterpret_cast<WindowLayout*>(refData);
        if (message == WM_NCDESTROY)
        {
            const LRESULT destroyed = DefSubclassProc(hwnd, message, wParam, lParam);
            RemoveWindowSubclass(hwnd, &SubclassProc, kSubclassId);
            if (self)
            {
                self->m_subclassed = false;
                // Ultima azione: la callback può distruggere questo stesso oggetto.
                if (const auto callback = self->onMainDestroyed)
                {
                    callback();
                }
            }
            return destroyed;
        }

        if (message == WM_NCLBUTTONDOWN && wParam == HTCAPTION && self && self->m_active &&
            (self->IsInHiddenTabsArea(lParam) || self->IsInPanelCaption(lParam) || self->IsInMenuBarHole(lParam)))
        {
            // Qui HTCAPTION lo diamo noi (WM_NCHITTEST più sotto). Per Windows un clic sulla barra del titolo apre subito il
            // ciclo di spostamento della finestra, anche senza trascinare, e l'input di Windows chiede all'editor dove sta il
            // cursore lampeggiante (WM_IME_REQUEST, IMR_QUERYCHARPOSITION): la RichEdit lo riporta in vista e il testo salta
            // (lo fa anche il Blocco note originale cliccando la sua barra del titolo). Qui la finestra passa in primo piano
            // alla pressione, come sempre, ma si sposta solo trascinando: il semplice clic finisce qui e il testo resta dov'è.
            // Trascinamento, doppio clic (ingrandisci) e clic destro (menu di sistema) restano di Windows.
            if (GetForegroundWindow() != hwnd)
            {
                SetForegroundWindow(hwnd);
            }
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!DragDetect(hwnd, point))
            {
                return 0;
            }
        }

        // La finestra principale del Blocco note non ha WS_CLIPCHILDREN: quando ridipinge il proprio sfondo copre anche le
        // figlie disegnate nella sua superficie, cioè l'editor (le isole XAML sono composte sopra e non ne risentono). Di
        // solito poi l'editor si ridisegna, ma sta su un altro thread e a volte lo fa prima: dopo un cambio di layout (pannello
        // compresso/espanso) la riga del titolo ridipinta lasciava una fascia scura sugli 8 px in cima all'editor salito.
        // Dopo ogni pittura della finestra principale si invalida la parte dell'editor che ha coperto (si ridisegna dopo).
        HRGN painted = nullptr;
        if (message == WM_PAINT && self && self->m_active && self->m_panelShown)
        {
            painted = CreateRectRgn(0, 0, 0, 0);
            if (painted && GetUpdateRgn(hwnd, painted, FALSE) <= NULLREGION)
            {
                DeleteObject(painted);
                painted = nullptr;
            }
        }
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (painted)
        {
            if (self->m_active)
            {
                self->RepaintEditorUnder(painted);
            }
            DeleteObject(painted);
        }
        if (self)
        {
            // Pulsanti della finestra nostri (CaptionButtons): più chiari con la finestra non attiva, tema riletto.
            if (message == WM_NCACTIVATE)
            {
                self->m_buttons.SetActive(wParam != FALSE);
            }
            else if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE || message == WM_DWMCOLORIZATIONCOLORCHANGED)
            {
                self->m_buttons.ThemeChanged();
            }
        }
        if (!self || !self->m_active)
        {
            return result;
        }

        if (message == WM_ERASEBKGND && wParam && self->m_panelShown)
        {
            // Sfondo cancellato fuori da WM_PAINT (es. RedrawWindow con RDW_ERASENOW): stessa cosa, sull'area del DC.
            RECT box{};
            if (GetClipBox(reinterpret_cast<HDC>(wParam), &box) > NULLREGION)
            {
                if (const HRGN erased = CreateRectRgnIndirect(&box))
                {
                    self->RepaintEditorUnder(erased);
                    DeleteObject(erased);
                }
            }
        }
        if (message == WM_NCHITTEST && result == HTCLIENT &&
            (self->IsInHiddenTabsArea(lParam) || self->IsInPanelCaption(lParam) || self->IsInMenuBarHole(lParam)))
        {
            // Dove c'era la striscia schede (ora nascosta), nella fascia del titolo in cima alla colonna (fuori dai
            // pulsanti del pannello: lì l'isola non c'è) e nelle zone vuote della barra dei menu salita (buchi nella sua
            // regione) la finestra si trascina come una normale barra del titolo; doppio clic = ingrandisci, clic destro
            // = menu di sistema.
            return HTCAPTION;
        }
        if (message == WM_SIZE || message == WM_DPICHANGED)
        {
            // Il Blocco note ha appena ricollocato le sue finestre: correggiamo subito, senza sfarfallio.
            self->Check();
            if (!glass::Hooked())
            {
                // Senza aggancio il Blocco note reimposta da solo la sua zona vetro (ad esempio ingrandendo la finestra).
                glass::Reapply(hwnd);
            }
        }
        else if (message == WM_NCACTIVATE || message == WM_ACTIVATE || message == WM_WINDOWPOSCHANGED)
        {
            // All'attivazione il Blocco note ricalcola il suo layout anche senza cambiare misura (niente WM_SIZE) e rimette
            // l'editor al suo posto naturale. L'hook WinEvent ce lo diceva 30-60 ms dopo: il clic che aveva attivato la
            // finestra arrivava all'editor ancora lì, cioè righe più su (una selezione al posto del cursore), e l'editor
            // riportava in vista il cursore lampeggiante (il testo saltava). Si corregge dentro lo stesso messaggio.
            const int moves = self->m_moves;
            self->Check();
            if (self->m_moves != moves)
            {
                NPX_VLOG(L"WindowLayout: %s, %d finestre rimesse subito nel layout della mod.",
                         message == WM_NCACTIVATE ? L"WM_NCACTIVATE" : message == WM_ACTIVATE ? L"WM_ACTIVATE" : L"WM_WINDOWPOSCHANGED",
                         self->m_moves - moves);
            }
        }
        return result;
    }

    bool WindowLayout::IsInHiddenTabsArea(LPARAM screenPoint) const
    {
        if (!m_panelShown || IsEmpty(m_nativeTabsNatural))
        {
            return false;
        }
        POINT point{ GET_X_LPARAM(screenPoint), GET_Y_LPARAM(screenPoint) };
        ScreenToClient(m_main, &point);
        return PtInRect(&m_nativeTabsNatural, point) != FALSE;
    }

    bool WindowLayout::IsInPanelCaption(LPARAM screenPoint) const
    {
        if (!m_panelShown || IsEmpty(m_panelCaption))
        {
            return false;
        }
        POINT point{ GET_X_LPARAM(screenPoint), GET_Y_LPARAM(screenPoint) };
        ScreenToClient(m_main, &point);
        return PtInRect(&m_panelCaption, point) != FALSE;
    }

    bool WindowLayout::IsInMenuBarHole(LPARAM screenPoint) const
    {
        if (!m_panelShown || !m_regionHwnd || m_regionHoles.empty() || !IsWindow(m_regionHwnd))
        {
            return false;
        }
        RECT island{};
        GetWindowRect(m_regionHwnd, &island);
        const POINT point{ GET_X_LPARAM(screenPoint) - island.left, GET_Y_LPARAM(screenPoint) - island.top };
        return std::any_of(m_regionHoles.begin(), m_regionHoles.end(), [&](RECT const& hole) { return PtInRect(&hole, point) != FALSE; });
    }

    void WindowLayout::SetMenuBarHoles(std::vector<RECT> holes, SIZE islandSize)
    {
        m_menuBarHoles = std::move(holes);
        m_menuBarHolesSize = islandSize;
        ApplyMenuBarRegion();
    }

    void WindowLayout::ApplyMenuBarRegion()
    {
        const HWND target = m_active && m_menuBarHwnd && IsWindow(m_menuBarHwnd) ? m_menuBarHwnd : nullptr;
        std::vector<RECT> holes;
        if (target)
        {
            // Zone calcolate per un'isola della stessa misura (durante un ridimensionamento il layout XAML arriva dopo:
            // nel frattempo niente buchi, che potrebbero finire sopra un pulsante spostato).
            RECT island{};
            GetWindowRect(target, &island);
            if (std::abs((island.right - island.left) - m_menuBarHolesSize.cx) <= 2 && std::abs((island.bottom - island.top) - m_menuBarHolesSize.cy) <= 2)
            {
                holes = m_menuBarHoles;
            }
        }
        const bool same = target == m_regionHwnd && holes.size() == m_regionHoles.size() &&
                          std::equal(holes.begin(), holes.end(), m_regionHoles.begin(), [](RECT const& a, RECT const& b) { return Same(a, b); });
        if (same)
        {
            return;
        }
        if (m_regionHwnd && m_regionHwnd != target && IsWindow(m_regionHwnd))
        {
            SetWindowRgn(m_regionHwnd, nullptr, TRUE); // la barra dei menu torna intera
        }
        m_regionHwnd = target;
        m_regionHoles = holes;
        if (!target)
        {
            return;
        }
        if (holes.empty())
        {
            SetWindowRgn(target, nullptr, TRUE);
            return;
        }
        // Regione "infinita" a destra e in basso (se l'isola si allarga prima del nuovo calcolo non viene tagliata),
        // meno le zone vuote. La regione taglia anche il disegno dell'isola: nei buchi si vede la finestra principale, cioè
        // lo stesso materiale (la barra dei menu, con il pannello attivo, è trasparente).
        constexpr int kFar = 32767;
        HRGN region = CreateRectRgn(0, 0, kFar, kFar);
        for (RECT const& hole : holes)
        {
            if (HRGN cut = CreateRectRgnIndirect(&hole))
            {
                CombineRgn(region, region, cut, RGN_DIFF);
                DeleteObject(cut);
            }
        }
        if (!region || !SetWindowRgn(target, region, TRUE))
        {
            if (region)
            {
                DeleteObject(region);
            }
            m_regionHwnd = nullptr; // si riprova al prossimo controllo
            m_regionHoles.clear();
            return;
        }
        NPX_VLOG(L"WindowLayout: barra dei menu con %zu zone vuote trascinabili (la prima da x=%ld a x=%ld).", holes.size(), holes.front().left,
                 holes.front().right);
    }

    void WindowLayout::RepaintEditorUnder(HRGN painted) const
    {
        const HWND editor = FindEditor();
        if (!editor)
        {
            return;
        }
        const RECT rect = interop::RectInClientOf(editor, m_main);
        const HRGN covered = CreateRectRgnIndirect(&rect);
        if (!covered)
        {
            return;
        }
        if (CombineRgn(covered, covered, painted, RGN_AND) > NULLREGION)
        {
            // Coordinate client dell'editor; solo invalidazione (niente RDW_UPDATENOW: l'editor è su un altro thread).
            OffsetRgn(covered, -rect.left, -rect.top);
            RedrawWindow(editor, nullptr, covered, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        DeleteObject(covered);
    }

    void WindowLayout::SetGeometry(int captionPx, int contentTopPx)
    {
        const std::pair<int, int> geometry{ captionPx, contentTopPx };
        if (geometry == m_geometry)
        {
            return;
        }
        m_geometry = geometry;
        NPX_LOG(L"WindowLayout: colonna %s (fascia del titolo %d px, editor da %d px).", captionPx > 0 ? L"in cima alla finestra" : L"accanto all'editor",
                captionPx, contentTopPx);
        if (onPanelGeometry)
        {
            onPanelGeometry(captionPx, contentTopPx);
        }
    }

    HWND WindowLayout::FindEditor() const
    {
        // Il Blocco note crea una NotepadTextBox (ognuna con il proprio thread) per ogni documento
        // aperto e mostra solo quella della scheda attiva: ci interessa quella visibile.
        for (HWND child = GetWindow(m_main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!IsWindowVisible(child))
            {
                continue;
            }
            const std::wstring className = interop::ClassOf(child);
            if (className == L"NotepadTextBox" || className == L"RichEditD2DPT")
            {
                return child;
            }
        }
        // Editor annidato in un contenitore: si risale al figlio diretto della finestra principale.
        if (const HWND nested = interop::FindDescendantByClass(m_main, L"NotepadTextBox", true))
        {
            return DirectChildOf(m_main, nested);
        }
        return nullptr;
    }

    HWND WindowLayout::EditorFocusTarget() const
    {
        if (!m_main || !IsWindow(m_main))
        {
            return nullptr;
        }
        if (HWND surface = interop::FindDescendantByClass(m_main, L"RichEditD2DPT", true))
        {
            return surface;
        }
        return interop::FindDescendantByClass(m_main, L"NotepadTextBox", true);
    }

    RECT WindowLayout::NaturalRect(HWND hwnd, RECT const& current)
    {
        auto& tracked = m_tracked[hwnd];
        if (!IsEmpty(tracked.applied) && Same(current, tracked.applied))
        {
            return tracked.natural; // è ancora dove l'abbiamo messa noi
        }
        // Il Blocco note l'ha (ri)posizionata: questo è il nuovo rettangolo naturale.
        tracked.natural = current;
        tracked.applied = current;
        return current;
    }

    void WindowLayout::Move(HWND hwnd, RECT const& rect)
    {
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        // Le finestre di altri thread (gli editor) si spostano in modo asincrono: una chiamata
        // sincrona cross-thread può bloccare la UI o andare in deadlock.
        if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId())
        {
            flags |= SWP_ASYNCWINDOWPOS;
        }
        SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
        ++m_moves;
    }

    void WindowLayout::Check()
    {
        if (!m_active || m_inCheck || !m_main || !m_panel || !IsWindow(m_main) || IsIconic(m_main))
        {
            return;
        }
        m_inCheck = true;
        struct Reset
        {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{ m_inCheck };

        RECT client{};
        GetClientRect(m_main, &client);
        const int clientWidth = client.right - client.left;
        if (clientWidth <= 0)
        {
            return; // appena ripristinata da icona l'area client può essere vuota per un attimo: resta tutto com'è
        }

        // Nessun editor visibile (es. pagina Impostazioni del Blocco note): niente pannello. Durante un
        // cambio di scheda però l'editor uscente si nasconde prima che compaia il nuovo: il pannello
        // si nasconde solo se la situazione dura (il controllo periodico la ricontrolla).
        const HWND editor = FindEditor();
        if (!editor)
        {
            const ULONGLONG now = GetTickCount64();
            if (m_noEditorSince == 0)
            {
                m_noEditorSince = now;
            }
            if (m_panelShown && now - m_noEditorSince >= kNoEditorGraceMs)
            {
                RestoreChildren();
                glass::SetTopLimit(m_main, std::nullopt, m_glassFallbackTop);
                SetPanelShown(false);
            }
            return;
        }
        m_noEditorSince = 0;

        // La fascia verticale dell'editor (non la modifichiamo mai) delimita il pannello.
        const RECT editorNatural = NaturalRect(editor, interop::RectInClientOf(editor, m_main));
        const int bandTop = editorNatural.top;
        const int bandBottom = editorNatural.bottom;

        UINT dpi = GetDpiForWindow(m_main);
        if (dpi == 0)
        {
            dpi = USER_DEFAULT_SCREEN_DPI;
        }
        // paneWidth è la larghezza effettiva decisa dal pannello (anche quella, stretta, da compresso).
        const int requested = MulDiv(std::clamp(m_settings.paneWidth, Settings::kCollapsedPaneWidth, Settings::kMaxPaneWidth), dpi, 96);
        const int paneWidth = std::min(requested, clientWidth - MulDiv(kMinEditorWidthDip, dpi, 96));
        if (bandBottom - bandTop < 40 || paneWidth <= 0)
        {
            if (m_panelShown)
            {
                RestoreChildren();
                glass::SetTopLimit(m_main, std::nullopt, m_glassFallbackTop);
                SetPanelShown(false);
            }
            return;
        }
        // La colonna (a sinistra) parte dalla cima della finestra, come le schede verticali di Chrome: anche le figlie
        // sopra l'editor (la barra dei menu) si tolgono dalla colonna.
        constexpr int panelTop = 0;

        // La striscia schede nativa (resa invisibile) occupa la barra del titolo: la si sposta fuori
        // dall'area visibile senza nasconderla (il suo XAML continua a funzionare, i pulsanti restano
        // invocabili) e la sua zona torna a comportarsi come barra del titolo (WM_NCHITTEST).
        if (m_nativeTabs && IsWindow(m_nativeTabs) && IsWindowVisible(m_nativeTabs))
        {
            const RECT natural = NaturalRect(m_nativeTabs, interop::RectInClientOf(m_nativeTabs, m_main));
            m_nativeTabsNatural = natural;
            if (!m_nativeTabsPin)
            {
                HideNativeTabs(natural);
            }
        }

        // Riga del titolo nascosta (come Edge con "nascondi la barra del titolo"): con le schede nella colonna la riga
        // del titolo resta vuota, e la barra dei menu (l'isola larga e bassa appena sopra l'editor) sale lì, fino ai
        // pulsanti della finestra; l'editor e la sua barra di scorrimento verticale si allungano verso l'alto.
        // In cima restano 3 px: il bordo superiore, che ridimensiona.
        HWND menuBar = nullptr;
        RECT menuNatural{};
        for (const HWND child : DirectChildren(m_main))
        {
            if (child == m_panel || child == m_nativeTabs || child == editor || !IsWindowVisible(child) || interop::IsOverlay(child))
            {
                continue;
            }
            const RECT current = interop::RectInClientOf(child, m_main);
            if (IsEmpty(current))
            {
                continue;
            }
            const RECT natural = NaturalRect(child, current);
            if (std::abs(natural.bottom - bandTop) <= kBandTolerance && natural.top > 0 &&
                natural.bottom - natural.top < MulDiv(120, dpi, 96) && natural.right - natural.left > MulDiv(200, dpi, 96))
            {
                menuBar = child;
                menuNatural = natural;
                break;
            }
        }
        const int raise = menuBar ? std::max(0, static_cast<int>(menuNatural.top) - kTitleTopInsetPx) : 0;
        const int reserve = MulDiv(kCaptionReserveDip, dpi, 96);
        // Dove va l'editor (come nel ciclo qui sotto): serve alle isole sospese sopra di lui.
        RECT editorWanted = Transform(editorNatural, paneWidth);
        if (raise > 0)
        {
            editorWanted.top -= raise;
        }
        const int floatingGap = MulDiv(8, static_cast<int>(dpi), 96);
        const int floatingAnchor = MulDiv(32, static_cast<int>(dpi), 96);
        std::vector<HWND> floatingOverColumn; // isole sospese che sporgono sulla colonna: il pannello va sotto di loro

        // Tutte le figlie dirette che stanno nella fascia dell'editor vengono tolte dalla colonna.
        for (const HWND child : DirectChildren(m_main))
        {
            // Le nostre isole sovrapposte all'editor (es. "Torna su") si posizionano da sole.
            if (child == m_panel || child == m_nativeTabs || !IsWindowVisible(child) || interop::IsOverlay(child))
            {
                continue;
            }
            const RECT current = interop::RectInClientOf(child, m_main);
            if (IsEmpty(current))
            {
                continue;
            }
            const RECT natural = child == editor ? editorNatural : NaturalRect(child, current);
            if (natural.top < panelTop - kBandTolerance || natural.bottom > bandBottom + kBandTolerance)
            {
                // Fuori dalla fascia della colonna: la barra di stato. Se una finestra spostata da noi ne è uscita (il
                // Blocco note ha cambiato il suo layout) torna dove la vuole lui.
                if (const auto it = m_tracked.find(child);
                    it != m_tracked.end() && !IsEmpty(it->second.applied) && Same(current, it->second.applied) && !Same(current, natural))
                {
                    pin::Clear(child);
                    Move(child, natural);
                    it->second.applied = natural;
                }
                continue;
            }
            if (child != editor && child != menuBar && IsFloating(natural, editorNatural, floatingGap))
            {
                // Sospesa sopra l'editor (la barra di ricerca): spostata con lui, alla sua misura.
                const RECT placed = PlaceFloating(natural, editorNatural, editorWanted, raise, floatingAnchor, floatingGap);
                if (!Same(current, placed))
                {
                    Move(child, placed);
                }
                m_tracked[child].applied = placed;
                if (placed.left < editorWanted.left)
                {
                    floatingOverColumn.push_back(child);
                }
                continue;
            }
            RECT wanted = Transform(natural, paneWidth);
            if (raise > 0)
            {
                if (child == menuBar)
                {
                    // Nella riga del titolo, fino ai pulsanti della finestra.
                    OffsetRect(&wanted, 0, -raise);
                    wanted.right = std::max(wanted.left, std::min<LONG>(wanted.right, clientWidth - reserve));
                }
                else if (std::abs(natural.top - bandTop) <= kBandTolerance)
                {
                    wanted.top -= raise; // editor e barra di scorrimento verticale: si allungano verso l'alto
                }
            }
            if (!Same(current, wanted))
            {
                Move(child, wanted);
            }
            m_tracked[child].applied = wanted;
            // Il Blocco note rimette le sue finestre al loro posto a ogni attivazione (e a volte poco dopo): editor, barre
            // di scorrimento e barra dei menu restano ferme qui (vedi PositionPin.h).
            pin::Set(child, natural, wanted);
            FitInnerWindows(child, natural, wanted);
        }

        // Barra dei menu salita nella riga del titolo: le sue zone vuote si trascinano (regione con i buchi).
        m_menuBarHwnd = raise > 0 ? menuBar : nullptr;
        ApplyMenuBarRegion();
        // Pulsanti della finestra dalla cima della finestra al fondo della barra dei menu salita, con i glifi centrati sulla
        // sua riga (vedi CaptionButtons); senza riga del titolo nascosta quelli di DWM.
        const RECT buttonsRow = raise > 0 ? RECT{ 0, menuNatural.top - raise, client.right, menuNatural.bottom - raise } : RECT{};
        m_buttons.Update(m_main, buttonsRow);
        m_buttons.Verify();

        // Il nostro pannello nella colonna.
        const RECT panel{ 0, panelTop, paneWidth, bandBottom };
        if (!Same(interop::RectInClientOf(m_panel, m_main), panel))
        {
            SetWindowPos(m_panel, HWND_TOP, panel.left, panel.top, panel.right - panel.left, panel.bottom - panel.top,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        // Le isole sospese che non stanno nell'editor (finestra stretta) sporgono sulla colonna: il pannello va sotto di loro
        // (in fondo all'ordine Z, dove le tiene il Blocco note). Non le si porta in cima: sopra l'editor (WS_CLIPSIBLINGS)
        // gli toglierebbero la loro zona, e nei loro margini trasparenti (l'ombra della barra di ricerca) si vedeva il nero.
        if (!floatingOverColumn.empty())
        {
            SetWindowPos(m_panel, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            m_panelLowered = true;
        }
        else if (m_panelLowered)
        {
            // Barra chiusa (o di nuovo dentro l'editor): il pannello torna in cima, come sempre.
            SetWindowPos(m_panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            m_panelLowered = false;
        }
        // Fascia del titolo in cima alla colonna: alta quanto la barra dei menu salita (i pulsanti della colonna accanto
        // ai menu) o, se la barra dei menu non si trova, quanto i pulsanti della finestra (32 DIP); fin dove comincia
        // l'editor al massimo.
        const int contentTop = bandTop - raise;
        // Zona vetro (Mica) del Blocco note: arriva fino al fondo della barra dei menu, cioè all'inizio naturale
        // dell'editor. L'editor salito ci entrerebbe e lì DWM lo schiarisce (vedi Glass.h): la zona finisce dove comincia.
        m_glassFallbackTop = bandTop;
        glass::SetTopLimit(m_main, raise > 0 ? std::optional<int>(contentTop) : std::nullopt, bandTop);
        int caption = raise > 0 ? static_cast<int>(menuNatural.bottom) - raise - panelTop : MulDiv(32, static_cast<int>(dpi), 96);
        caption = std::clamp(caption, 0, std::max(0, contentTop - panelTop));
        m_panelCaption = caption > 0 ? RECT{ 0, panelTop, paneWidth, panelTop + caption } : RECT{};
        SetGeometry(caption, contentTop - panelTop); // prima di mostrarlo: compare già con la sua fascia
        SetPanelShown(true);
        if (onScrollBarsChange)
        {
            // L'isola della barra orizzontale, come l'editor, nel layout del Blocco note parte da x=0: Transform ne sposta
            // il bordo sinistro oltre la colonna, cioè la accorcia di paneWidth.
            onScrollBarsChange(raise, paneWidth);
        }
    }

    void WindowLayout::FitInnerWindows(HWND container, RECT const& containerNatural, RECT const& containerWanted)
    {
        // La superficie di testo (RichEditD2DPT) dentro il contenitore dell'editor non si adatta quando il
        // contenitore viene ristretto: resterebbe larga come prima, tagliata dal contenitore, e il testo
        // (a capo automatico compreso) finirebbe oltre il bordo visibile, sotto la barra di scorrimento.
        // Le finestre interne che sporgerebbero si restringono della stessa quantità, mantenendo il
        // proprio margine destro; quelle già dentro restano come sono.
        const int shrink = (containerNatural.right - containerNatural.left) - (containerWanted.right - containerWanted.left);
        const int wantedWidth = containerWanted.right - containerWanted.left;
        // Riga del titolo nascosta: il contenitore si allunga verso l'alto e le finestre interne che arrivavano in fondo
        // si allungano della stessa quantità (altrimenti in fondo all'editor resterebbe una striscia senza testo).
        const int grow = (containerWanted.bottom - containerWanted.top) - (containerNatural.bottom - containerNatural.top);
        const int naturalHeight = containerNatural.bottom - containerNatural.top;
        for (HWND inner = GetWindow(container, GW_CHILD); inner; inner = GetWindow(inner, GW_HWNDNEXT))
        {
            if (!IsWindowVisible(inner))
            {
                continue;
            }
            const RECT current = RectInParent(inner);
            if (IsEmpty(current))
            {
                continue;
            }
            const RECT natural = NaturalRect(inner, current);
            RECT wanted = natural;
            if (shrink > 0 && natural.right > wantedWidth)
            {
                wanted.right = std::max<LONG>(natural.left, natural.right - shrink);
            }
            if (grow > 0 && natural.bottom >= naturalHeight - kBandTolerance)
            {
                // Fino in fondo al contenitore allungato (se il Blocco note l'ha già allungata da sé, resta com'è).
                wanted.bottom = std::max<LONG>(natural.bottom, naturalHeight + grow);
            }
            if (!Same(current, wanted))
            {
                Move(inner, wanted);
            }
            m_tracked[inner].applied = wanted;
            pin::Set(inner, natural, wanted); // la superficie di testo: niente WM_SIZE quando il Blocco note la riafferma
        }
    }

    void WindowLayout::HideNativeTabs(RECT const& natural)
    {
        RECT hidden = natural;
        OffsetRect(&hidden, -(natural.right + kOffscreenMargin), 0);
        if (!Same(interop::RectInClientOf(m_nativeTabs, m_main), hidden))
        {
            Move(m_nativeTabs, hidden);
        }
        m_tracked[m_nativeTabs].applied = hidden;
    }

    void WindowLayout::PinNativeTabs(std::optional<POINT> clientTopLeft)
    {
        m_nativeTabsPin = clientTopLeft;
        if (!m_nativeTabs || !IsWindow(m_nativeTabs) || !m_main)
        {
            return;
        }

        if (clientTopLeft)
        {
            const RECT current = interop::RectInClientOf(m_nativeTabs, m_main);
            const RECT pinned{ clientTopLeft->x, clientTopLeft->y, clientTopLeft->x + (current.right - current.left),
                               clientTopLeft->y + (current.bottom - current.top) };
            // In fondo all'ordine Z: è invisibile e non deve intercettare il mouse.
            SetWindowPos(m_nativeTabs, HWND_BOTTOM, pinned.left, pinned.top, pinned.right - pinned.left, pinned.bottom - pinned.top,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            m_tracked[m_nativeTabs].applied = pinned;
            return;
        }

        // Di nuovo fuori vista (o al suo posto, se nel frattempo il layout è stato disattivato).
        const auto it = m_tracked.find(m_nativeTabs);
        if (it == m_tracked.end() || IsEmpty(it->second.natural))
        {
            return;
        }
        if (m_active)
        {
            HideNativeTabs(it->second.natural);
        }
        else
        {
            Move(m_nativeTabs, it->second.natural);
        }
    }

    void WindowLayout::RestoreChildren()
    {
        for (auto const& [hwnd, tracked] : m_tracked)
        {
            pin::Clear(hwnd); // prima di rimetterla al posto naturale: la regola lo impedirebbe
            if (!IsWindow(hwnd) || IsEmpty(tracked.natural))
            {
                continue;
            }
            const RECT current = RectInParent(hwnd); // figlie dirette e finestre interne degli editor
            // Solo se è ancora dove l'avevamo messa noi (altrimenti il Blocco note l'ha già ricollocata).
            if (Same(current, tracked.applied) && !Same(tracked.applied, tracked.natural))
            {
                Move(hwnd, tracked.natural);
            }
        }
        m_tracked.clear();
        m_nativeTabsNatural = {};
        m_nativeTabsPin.reset();
        m_panelCaption = {};
        m_menuBarHwnd = nullptr;
        ApplyMenuBarRegion(); // barra dei menu di nuovo intera
        if (m_main && IsWindow(m_main))
        {
            m_buttons.Update(m_main, RECT{}); // di nuovo i pulsanti di DWM
        }
        if (onScrollBarsChange)
        {
            onScrollBarsChange(0, 0); // editor e barre di scorrimento di nuovo come le vuole il Blocco note
        }
    }

    void WindowLayout::RequestNativeRelayout()
    {
        if (!IsWindow(m_main) || IsIconic(m_main))
        {
            return;
        }
        // Il Blocco note ricalcola il proprio layout su WM_SIZE. Lo si accoda (non SendMessage)
        // e con il tipo corretto anche per la finestra massimizzata.
        RECT client{};
        GetClientRect(m_main, &client);
        PostMessageW(m_main, WM_SIZE, IsZoomed(m_main) ? SIZE_MAXIMIZED : SIZE_RESTORED,
                     MAKELPARAM(client.right - client.left, client.bottom - client.top));
    }

    void WindowLayout::SetPanelShown(bool shown)
    {
        if (m_panelShown == shown)
        {
            return;
        }
        m_panelShown = shown;
        if (onPanelVisibility)
        {
            onPanelVisibility(shown);
        }
    }
}
