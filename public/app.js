// app.js - opens the Login / Sign Up popups (modals) and the mobile menu drawer, and shows the live password checklist + eye button.
//
// How it works:
//  - The navbar links "Login" and "Sign Up" are normal links (href="/login", href="/signup").
//    They also carry data-modal="loginModal" / data-modal="signupModal".
//  - If this script runs, a click on such a link opens the popup with that id instead of loading the page.
//  - If JavaScript is off (or this file did not load) nothing changes: the links open the /login and /signup pages.
//  - The popup forms POST to the same /login and /signup routes as those pages.

(function () {
  // show the popup with this id; returns false if there is no such popup on this page
  function openModal(id) {
    var modal = document.getElementById(id);
    if (!modal) return false;
    closeAllModals();
    modal.classList.add('open');
    var firstInput = modal.querySelector('input');
    if (firstInput) firstInput.focus();        // the user can start typing at once
    return true;
  }

  // hide every open popup
  function closeAllModals() {
    var open = document.querySelectorAll('.modal.open');
    for (var i = 0; i < open.length; i++) {
      open[i].classList.remove('open');
    }
  }

  // mobile menu drawer: html.drawer-open shows it. Without JavaScript the same drawer opens with :target (href="#menu")
  function setDrawer(open) {
    document.documentElement.classList.toggle('drawer-open', open);
  }

  // ---- password forms: live checklist and the show / hide eye ----
  // The real rules are on the server (check_password in pages.cpp); this only shows them while typing. Without JavaScript
  // the checklist just stays grey, the eye button stays hidden, and the server still checks everything on submit.
  var MARK_OK = '\u2713', MARK_BAD = '\u2717';

  // pieces of the name / email that the password must not contain (3+ characters, lower case): same as the server
  function personalBits(form) {
    var nameEl = form.querySelector('[name="name"]');
    var emailEl = form.querySelector('[name="email"]');
    var name = (nameEl ? nameEl.value : form.getAttribute('data-name') || '').toLowerCase();
    var email = (emailEl ? emailEl.value : form.getAttribute('data-email') || '').toLowerCase();
    var bits = [], words = name.split(/[ \t]+/), joined = '';
    for (var i = 0; i < words.length; i++) {
      if (words[i].length >= 3) bits.push(words[i]);
      joined += words[i];
    }
    if (joined.length >= 3) bits.push(joined);
    var local = email.indexOf('@') >= 0 ? email.substring(0, email.indexOf('@')) : email;
    if (local.length >= 3) bits.push(local);
    return bits;
  }

  function setupRules(list) {
    var form = list.closest('form');
    var nw = form.querySelector('[data-pw="new"]');
    if (!nw) return;
    var cf = form.querySelector('[data-pw="confirm"]');
    var old = form.querySelector('[data-pw="old"]');
    var min = parseInt(list.getAttribute('data-min'), 10), max = parseInt(list.getAttribute('data-max'), 10);
    var common = (list.getAttribute('data-common') || '').split('|');

    function update() {
      var pw = nw.value, low = pw.toLowerCase(), has = pw !== '';
      var len = pw.replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]/g, 'x').length;       // characters, not UTF-16 units
      var personal = true, bits = personalBits(form);
      for (var i = 0; i < bits.length; i++) if (low.indexOf(bits[i]) >= 0) personal = false;
      var r = {
        len: len >= min && len <= max,
        space: has && !/[\x00-\x20\x7f]/.test(pw),
        upper: /[A-Z]/.test(pw),
        lower: /[a-z]/.test(pw),
        digit: /[0-9]/.test(pw),
        special: /[\x21-\x2f\x3a-\x40\x5b-\x60\x7b-\x7e]/.test(pw),
        personal: has && personal,
        common: has && common.indexOf(low) < 0,
        different: has && old ? pw !== old.value : has,
        match: has && !!cf && pw === cf.value
      };
      var items = list.querySelectorAll('li[data-rule]');
      var allOk = true;
      for (var k = 0; k < items.length; k++) {
        var ok = !!r[items[k].getAttribute('data-rule')];
        if (!ok) allOk = false;
        items[k].className = ok ? 'ok' : 'bad';
        items[k].querySelector('.pw-mark').textContent = ok ? MARK_OK : MARK_BAD;
      }
      // The list is hidden until it is needed: shown while a password field is focused or has text, hidden again as soon as every
      // rule passes, or when the password is empty and the focus has left the fields.
      var focused = document.activeElement === nw || document.activeElement === cf;
      var filled = nw.value !== '' || (!!cf && cf.value !== '');
      list.classList.toggle('open', !allOk && (focused || filled));
    }

    // After a server error the page comes with the list already open and only the FAILED rules visible (class "server").
    // It stays like that until the user touches a password field; then the normal live list takes over.
    function leaveServerMode() {
      if (list.classList.contains('server')) list.classList.remove('server');
    }
    function onTouch() {
      leaveServerMode();
      update();
    }
    function onLeave() {
      setTimeout(update, 0);                    // activeElement is only the new field after the blur event is over
    }

    var inputs = form.querySelectorAll('input');
    for (var j = 0; j < inputs.length; j++) inputs[j].addEventListener('input', onTouch);
    var pwFields = [nw, cf];
    for (var f = 0; f < pwFields.length; f++) {
      if (!pwFields[f]) continue;
      pwFields[f].addEventListener('focus', onTouch);
      pwFields[f].addEventListener('blur', onLeave);
    }
    if (!list.classList.contains('server')) update();
  }

  function initPasswordForms() {
    document.documentElement.classList.add('js');       // css: with JavaScript the one-line hint is replaced by the live list
    var lists = document.querySelectorAll('.pw-rules');
    for (var i = 0; i < lists.length; i++) setupRules(lists[i]);
    var eyes = document.querySelectorAll('.pw-eye');
    for (var j = 0; j < eyes.length; j++) eyes[j].hidden = false;
  }
  initPasswordForms();

  // one click listener for the whole page
  document.addEventListener('click', function (event) {
    var target = event.target;
    if (!target.closest) return;

    // the eye button: show / hide the password in the input just before it
    var eye = target.closest('.pw-eye');
    if (eye) {
      var field = eye.previousElementSibling;
      var show = field.type === 'password';
      field.type = show ? 'text' : 'password';
      eye.setAttribute('aria-pressed', show ? 'true' : 'false');
      eye.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
      return;
    }

    // 0. the menu button / tab opens the drawer; the x and the dark background close it, and so does any link inside it
    var drawerBtn = target.closest('[data-drawer]');
    if (drawerBtn) {
      event.preventDefault();
      setDrawer(drawerBtn.getAttribute('data-drawer') === 'open');
      return;
    }
    if (target.closest('.drawer-panel a')) setDrawer(false);     // a data-modal link also goes on to step 1

    // 1. a link/button with data-modal="someId": open that popup (and do not follow the link)
    var opener = target.closest('[data-modal]');
    if (opener && openModal(opener.getAttribute('data-modal'))) {
      event.preventDefault();
      return;
    }

    // 2. the x button (data-close) closes the popup
    if (target.closest('[data-close]')) {
      closeAllModals();
      return;
    }

    // 3. a click on the dark background (the .modal itself, not the white box) closes the popup
    if (target.classList.contains('modal')) {
      closeAllModals();
    }
  });

  // Escape closes the popup
  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape') { closeAllModals(); setDrawer(false); }
  });
})();
