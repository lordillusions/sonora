// Sonora website: mobile menu, screenshot lightbox and the download for the visitor's system. No tracking, no external
// requests: the system is read from the browser and only picks which link is shown first.
(function ()
{
    var toggle = document.querySelector(".nav-toggle");
    var nav = document.getElementById("nav");
    if (toggle && nav)
    {
        toggle.addEventListener("click", function ()
        {
            var open = nav.classList.toggle("open");
            toggle.setAttribute("aria-expanded", open ? "true" : "false");
        });
        document.addEventListener("keydown", function (e)
        {
            if (e.key === "Escape" && nav.classList.contains("open"))
            {
                nav.classList.remove("open");
                toggle.setAttribute("aria-expanded", "false");
                toggle.focus();
            }
        });
    }

    var box = document.createElement("div");
    box.className = "lightbox";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    box.setAttribute("aria-label", "Screenshot, enlarged. Press Escape to close.");
    box.tabIndex = -1;
    var big = document.createElement("img");
    big.alt = "";
    box.appendChild(big);
    document.body.appendChild(box);

    var opener = null;
    function close()
    {
        if (!box.classList.contains("open")) return;
        box.classList.remove("open");
        big.removeAttribute("src");
        if (opener) opener.focus();
    }
    function open(img)
    {
        if (window.innerWidth < 640) return; // phones already show the image full width
        opener = img;
        big.src = img.currentSrc || img.src;
        big.alt = img.alt;
        box.classList.add("open");
        box.focus();
    }
    box.addEventListener("click", close);
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") close(); });

    document.querySelectorAll(".shot img").forEach(function (img)
    {
        // Screenshots can be enlarged with the mouse or the keyboard.
        img.tabIndex = 0;
        img.setAttribute("role", "button");
        img.setAttribute("aria-label", "Enlarge screenshot: " + img.alt);
        img.addEventListener("click", function () { open(img); });
        img.addEventListener("keydown", function (e)
        {
            if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open(img); }
        });
    });

    var year = document.getElementById("year");
    if (year) year.textContent = String(new Date().getFullYear());

    // Download for this computer: "mac", "windows", "linux", or null (phones, tablets, unknown).
    function detectSystem()
    {
        var ua = navigator.userAgent || "";
        var platform = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
        if (/iPhone|iPad|iPod|Android/i.test(ua) || (/Mac/i.test(platform) && navigator.maxTouchPoints > 1)) return null;
        if (/Mac/i.test(platform) || /Mac OS X/.test(ua)) return "mac";
        if (/Win/i.test(platform) || /Windows/.test(ua)) return "windows";
        if (/Linux|X11/i.test(platform) || /Linux/.test(ua)) return "linux";
        return null;
    }
    var systems = {
        mac: { name: "Mac", note: "Sonora for Mac needs macOS 13 or later on an Apple silicon Mac (M1 or newer)." },
        windows: { name: "Windows", note: "Early alpha for Windows 10 and 11, 64-bit. Windows will warn about an unknown publisher." },
        linux: { name: "Linux", note: "Early alpha AppImage for 64-bit Linux. A .deb package for Debian and Ubuntu is below." }
    };
    var system = detectSystem();
    document.querySelectorAll("[data-os-label]").forEach(function (link)
    {
        if (system) link.textContent = link.getAttribute("data-os-label") + " " + systems[system].name;
    });
    var card = system && document.querySelector('.dl-card[data-os="' + system + '"]');
    var primary = document.getElementById("dl-primary");
    if (card)
    {
        card.classList.add("main");
        var tag = document.createElement("span");
        tag.className = "badge badge-soft dl-yours";
        tag.textContent = "Your system";
        card.insertBefore(tag, card.firstChild);
        var first = card.querySelector("a.btn");
        first.classList.remove("btn-ghost");
        first.classList.add("btn-primary");
        card.parentNode.insertBefore(card, card.parentNode.firstChild); // shown first
        if (primary)
        {
            primary.href = first.href;
            primary.textContent = "Download for " + systems[system].name;
            document.getElementById("dl-note").textContent = systems[system].note + " Not your system? See all downloads below.";
        }
    }
})();
