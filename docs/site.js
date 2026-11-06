// Vertical Tabs sites: language (EN/IT), collapsible tab column, active tab while scrolling.
// <html data-lang> and <html data-collapsed> are already set by the inline script in <head>.
(function () {
  var root = document.documentElement;
  var narrow = window.matchMedia('(max-width: 760px)');

  function store(key, value) { try { localStorage.setItem(key, value); } catch (e) { /* private mode */ } }

  // ---- Language
  var langButtons = document.querySelectorAll('[data-set-lang]');
  function setLang(lang) {
    root.lang = lang;
    root.setAttribute('data-lang', lang);
    langButtons.forEach(function (b) { b.setAttribute('aria-pressed', String(b.getAttribute('data-set-lang') === lang)); });
    store('vt-lang', lang);
  }
  langButtons.forEach(function (b) { b.addEventListener('click', function () { setLang(b.getAttribute('data-set-lang')); }); });
  setLang(root.getAttribute('data-lang') === 'it' ? 'it' : 'en');

  // ---- Collapse (on narrow screens the open column is an overlay that closes after a choice)
  var toggle = document.querySelector('.collapse');
  function setCollapsed(collapsed, remember) {
    root.setAttribute('data-collapsed', String(collapsed));
    toggle.setAttribute('aria-expanded', String(!collapsed));
    if (remember) store('vt-collapsed', String(collapsed));
  }
  toggle.addEventListener('click', function () {
    setCollapsed(root.getAttribute('data-collapsed') !== 'true', !narrow.matches);
  });
  document.querySelectorAll('.tab, .newtab').forEach(function (a) {
    a.addEventListener('click', function () { if (narrow.matches) setCollapsed(true, false); });
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && narrow.matches && root.getAttribute('data-collapsed') === 'false') { setCollapsed(true, false); toggle.focus(); }
  });
  narrow.addEventListener('change', function (e) {
    var saved = null;
    try { saved = localStorage.getItem('vt-collapsed'); } catch (err) { /* private mode */ }
    setCollapsed(e.matches ? true : saved === 'true', false);
  });
  setCollapsed(root.getAttribute('data-collapsed') === 'true', false);

  // ---- The tab of the section being read is the active one
  var links = {};
  document.querySelectorAll('.tab[href^="#"]').forEach(function (a) { links[a.getAttribute('href').slice(1)] = a; });
  function activate(id) {
    Object.keys(links).forEach(function (key) {
      if (key === id) links[key].setAttribute('aria-current', 'location');
      else links[key].removeAttribute('aria-current');
    });
  }
  var sections = Array.prototype.filter.call(document.querySelectorAll('main section[id]'), function (s) { return links[s.id]; });
  if ('IntersectionObserver' in window && sections.length) {
    var visible = {};
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) { visible[entry.target.id] = entry.isIntersecting; });
      var first = sections.filter(function (s) { return visible[s.id]; })[0];
      if (first) activate(first.id);
    }, { rootMargin: '-35% 0px -60% 0px' });
    sections.forEach(function (s) { observer.observe(s); });
  }
  // At the bottom of the page the last section may be too short to reach the band: it wins there
  window.addEventListener('scroll', function () {
    if (sections.length && window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4) activate(sections[sections.length - 1].id);
  }, { passive: true });
  activate((location.hash || '#' + (sections[0] && sections[0].id)).slice(1));

  // The "new tab" link opens the other project in the same language
  var newtab = document.querySelector('.newtab');
  if (newtab) {
    var base = newtab.getAttribute('href').split('?')[0];
    var sync = function () { newtab.setAttribute('href', base + '?lang=' + root.getAttribute('data-lang')); };
    langButtons.forEach(function (b) { b.addEventListener('click', sync); });
    sync();
  }
})();
