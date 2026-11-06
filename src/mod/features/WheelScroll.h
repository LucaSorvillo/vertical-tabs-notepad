#pragma once

#include "Settings.h"

namespace npx
{
    // "Disattiva lo scorrimento fluido": la rotella sul testo sposta subito il documento delle righe impostate in Windows
    // (come il vecchio Blocco note), invece dell'animazione dell'editor. L'editor è la RichEdit di Office (riched20.dll nel
    // pacchetto del Blocco note): a ogni scatto scorre a piccoli passi per ~0,3 s (un timer da ~30 ms). L'animazione è sempre
    // attiva: la RichEdit non guarda le impostazioni di animazione di Windows. Solo la rotella verticale sul testo è fluida;
    // tastiera, rotella orizzontale e barra di scorrimento erano già istantanee.
    //
    // L'editor di ogni scheda (NotepadTextBox > RichEditD2DPT) vive su un thread suo. Su ognuno si mette un hook WH_GETMESSAGE
    // (stesso processo: hMod nullo). Il WM_MOUSEWHEEL diretto alla superficie di testo, senza Ctrl (zoom) né Maiusc, diventa
    // WM_NULL e l'hook scorre subito: EM_LINESCROLL delle righe di Windows (SPI_GETWHEELSCROLLLINES; "una schermata" =
    // SB_PAGEUP/SB_PAGEDOWN). Se l'editor non si muove (righe rifiutate: visto su una scheda con zoom e "a capo automatico",
    // in uno stato interno della RichEdit), stessa distanza a pixel con EM_SETSCROLLPOS, ma mai oltre la fine vera del testo
    // (nMax - nPage, 0 se il testo ci sta): EM_SETSCROLLPOS non la rispetta e portava il testo nel vuoto in fondo al
    // documento (o in uno che ci sta in altezza), da dove il Blocco note lo riportava indietro a scatti. Se nemmeno così si
    // muove (in fondo, o corsa sconosciuta), lo scatto passa al Blocco note com'era (animazione, che sblocca anche quello
    // stato; in fondo resta fermo). La rotella non resta mai senza effetto per colpa nostra. Le frazioni di scatto
    // (touchpad, rotelle ad alta risoluzione) si accumulano.
    //
    // Gli hook si aggiungono dal timer della finestra (schede aperte dopo) e si tolgono spegnendo la funzione, chiudendo la
    // finestra o quando l'editor sparisce. Vive e viene usata sul thread UI della finestra; la procedura dell'hook gira sui
    // thread degli editor.
    class WheelScroll
    {
    public:
        explicit WheelScroll(winrt::NotepadXamlUI::TabsBar const& tabsBar) : m_tabsBar(winrt::make_weak(tabsBar))
        {
        }
        ~WheelScroll();
        WheelScroll(WheelScroll const&) = delete;
        WheelScroll& operator=(WheelScroll const&) = delete;

        void Apply(Settings const& settings);
        // Controllo periodico (500 ms): editor nuovi (schede aperte) da agganciare, editor spariti.
        void OnTimer();
        void Detach();

    private:
        HWND MainWindow();
        void HookEditors();
        void UnhookAll();

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        HWND m_main = nullptr;
        bool m_enabled = false;
        std::unordered_map<DWORD, HHOOK> m_hooks; // thread dell'editor -> hook
        bool m_failureLogged = false;
    };
}
