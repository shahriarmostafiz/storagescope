(function () {
  var REPO = "shahriarmostafiz/storagescope";
  var root = document.documentElement;

  // Theme toggle (remembered; falls back to the system setting).
  try { var t = localStorage.getItem("theme"); if (t) root.setAttribute("data-theme", t); } catch (e) {}
  document.getElementById("theme").addEventListener("click", function () {
    var dark = root.getAttribute("data-theme") === "dark" ||
      (!root.getAttribute("data-theme") && matchMedia("(prefers-color-scheme: dark)").matches);
    var next = dark ? "light" : "dark";
    root.setAttribute("data-theme", next);
    try { localStorage.setItem("theme", next); } catch (e) {}
  });


  // Click (or Enter/Space) on a screenshot opens it full size; click, Esc, or the close button closes it.
  var lb = document.createElement("div"), lbImg = document.createElement("img"), lbCap = document.createElement("p"), lbX = document.createElement("button"), lastFocus = null;
  lb.className = "lb"; lb.hidden = true; lb.setAttribute("role", "dialog"); lb.setAttribute("aria-modal", "true"); lb.setAttribute("aria-label", "Screenshot");
  lbX.type = "button"; lbX.textContent = "×"; lbX.setAttribute("aria-label", "Close");
  lb.appendChild(lbX); lb.appendChild(lbImg); lb.appendChild(lbCap); document.body.appendChild(lb);
  function closeLb() { lb.hidden = true; document.body.style.overflow = ""; if (lastFocus) lastFocus.focus(); }
  function openLb(img) {
    lastFocus = img; lbImg.src = img.currentSrc || img.src; lbImg.alt = img.alt; lbCap.textContent = img.alt;
    lb.hidden = false; document.body.style.overflow = "hidden"; lbX.focus();
  }
  document.addEventListener("click", function (e) { var t = e.target; if (t.classList && t.classList.contains("zoom")) openLb(t); });
  document.addEventListener("keydown", function (e) {
    if (!lb.hidden && e.key === "Escape") closeLb();
    else if (lb.hidden && (e.key === "Enter" || e.key === " ") && e.target.classList && e.target.classList.contains("zoom")) { e.preventDefault(); openLb(e.target); }
  });
  lb.addEventListener("click", closeLb);

  // Two-page SPA on hash routes: #/ (details + download) and #/features. #/download and #/<section-id> open Home and scroll.
  var pages = { home: document.getElementById("page-home"), features: document.getElementById("page-features") };
  var navLinks = document.querySelectorAll("[data-route]");
  function route() {
    var seg = location.hash.replace(/^#\/?/, "").split("?")[0];
    var page = seg === "features" ? "features" : "home";
    var target = page === "home" && seg ? document.getElementById(seg) : null;
    Object.keys(pages).forEach(function (k) { pages[k].hidden = k !== page; });
    document.title = pages[page].getAttribute("data-title");
    var cur = page === "features" ? "features" : (seg === "download" ? "download" : "home");
    navLinks.forEach(function (a) { if (a.getAttribute("data-route") === cur) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current"); });
    if (target && pages.home.contains(target)) target.scrollIntoView(); else window.scrollTo(0, 0);
    document.getElementById("top").focus({ preventScroll: true });
  }
  window.addEventListener("hashchange", route);
  route();

  function mb(n) { return (n / 1048576).toFixed(2) + " MB"; }
  function day(s) { return new Date(s).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }); }
  function exe(rel) { return (rel.assets || []).filter(function (a) { return /\.exe$/i.test(a.name); })[0]; }
  function $(id) { return document.getElementById(id); }
  function text(el, s) { el.textContent = s; }
  function show(el, s) { if (s) { text(el, s); el.hidden = false; } else el.hidden = true; }

  // Short plain-text description from the release notes (no HTML is ever inserted from GitHub data).
  function notes(body) {
    var lines = (body || "").split(/\r?\n/).map(function (l) {
      return l.replace(/\[([^\]]*)\]\([^)]*\)/g, "$1").replace(/[#*`>_-]+/g, " ").replace(/\s+/g, " ").trim();
    }).filter(function (l) { return l && !/sha.?256|checksum|https?:\/\//i.test(l); });
    var s = lines.slice(0, 2).join(" · ");
    return s.length > 240 ? s.slice(0, 237) + "..." : s;
  }

  var rels = [];
  function select(i) {
    var r = rels[i], a = exe(r), m = /[a-f0-9]{64}/i.exec(r.body || "");
    $("dl-main2").href = a.browser_download_url;
    $("dl-main2").textContent = "Download " + r.tag_name;
    text($("dl-ver"), r.name || r.tag_name);
    text($("dl-info"), day(r.published_at) + " · " + mb(a.size) + " · Windows 10 and 11, 64-bit, portable");
    show($("dl-sha"), m ? "SHA-256: " + m[0] : "");
    var sa = (r.assets || []).filter(function (x) { return /\.sha256$/i.test(x.name); })[0];
    if (!m && sa) {
      fetch(sa.browser_download_url).then(function (x) { return x.text(); }).then(function (t) {
        var h = /[a-f0-9]{64}/i.exec(t);
        if (h && rels[parseInt($("ver").value, 10)] === r) show($("dl-sha"), "SHA-256: " + h[0]);
      }).catch(function () {});
    }
    show($("dl-notes"), notes(r.body));
    show($("dl-old"), i > 0 ? "This is not the latest version. Newer releases may include fixes." : "");
  }

  function render(list) {
    // Stable releases with an exe attached, newest first (the API returns them in that order).
    rels = list.filter(function (r) { return !r.draft && !r.prerelease && exe(r); }).slice(0, 15);
    if (!rels.length) { fail("No release is published yet. Check back soon, or see the GitHub releases page."); return; }
    var latest = rels[0], la = exe(latest);
    $("dl-main").href = la.browser_download_url;
    text($("dl-meta"), "Version " + latest.tag_name + " · " + mb(la.size) + " · Windows 10 and 11, 64-bit");
    var sel = $("ver");
    sel.textContent = "";
    rels.forEach(function (r, i) {
      var o = document.createElement("option");
      o.value = String(i); text(o, (r.name || r.tag_name) + (i === 0 ? " (latest)" : "") + " · " + day(r.published_at));
      sel.appendChild(o);
    });
    sel.disabled = rels.length < 2;
    sel.onchange = function () { select(parseInt(sel.value, 10)); };
    select(0);
  }

  function fail(msg) {
    $("ver").hidden = true; $("ver-label").hidden = true;
    text($("dl-ver"), "Latest release");
    text($("dl-info"), msg || "Couldn't load the release list. Use the GitHub releases link below.");
  }

  // Cache for 10 minutes: the unauthenticated GitHub API allows 60 requests per hour per visitor.
  var key = "ss-releases", cached = null;
  try { cached = JSON.parse(localStorage.getItem(key)); } catch (e) {}
  if (cached && Date.now() - cached.t < 600000) { render(cached.d); return; }
  fetch("https://api.github.com/repos/" + REPO + "/releases?per_page=30", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { if (!r.ok) throw 0; return r.json(); })
    .then(function (d) { try { localStorage.setItem(key, JSON.stringify({ t: Date.now(), d: d })); } catch (e) {} render(d); })
    .catch(function () { if (cached) render(cached.d); else fail(); });
})();
