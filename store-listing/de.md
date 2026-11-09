ArborTab – Baum-Tabs in Chromes nativer vertikaler Tableiste

Nur Windows x64. Erfordert Chromes native vertikale Tableiste. Für die vollständige visuelle Integration ist ein separates quelloffenes natives Modul von GitHub nötig. Lesen Sie vor dessen Installation den Sicherheitshinweis.

ArborTab organisiert Chrome-Tabs als echte Eltern-Kind-Struktur direkt in Chromes eigener vertikaler Tableiste – ohne Seitenleiste und ohne zweite Tab-Liste.

Funktionen:

- Über Links geöffnete Tabs werden Kinder des öffnenden Tabs.
- Ganze Teilbäume ein- und ausklappen.
- Einen Eltern-Tab samt aller Nachkommen schließen.
- Tabs per Drag-and-drop verschachteln oder Geschwister neu anordnen.
- Einrückungslinien, Anzahl ausgeblendeter Tabs, Theme-Farben und optionale Animationen.
- Tastenkürzel zum Ein-/Ausklappen, Schließen eines Baums sowie Ein- und Ausrücken.

Die Erweiterung allein verwaltet den Baum, ordnet Tabs, unterstützt Tastenkürzel und schließt Teilbäume. Ohne natives Modul bleibt Chromes Tableiste optisch flach.

Installation des nativen Moduls:

Öffnen Sie nach der Erweiterungsinstallation die ArborTab-Einstellungen. Dort finden Sie den Link zum neuesten GitHub-Release und den Installationsbefehl. Quellcode, Skripte, Pakete, Prüfsummen und Deinstallation:
https://github.com/EvATive7/chrome-plugin-arbortab

Installationsbefehl:
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

Sicherheitshinweis:

Die native Komponente ist ein `version.dll`-Proxy neben `chrome.exe` und greift in Layout, Darstellung, Maus- und Drag-Verarbeitung der vertikalen Tableiste ein. Da die verteilte DLL nicht von Microsoft signiert ist, deaktiviert sie für Chrome-Kindprozesse die Beschränkung auf ausschließlich Microsoft-signierte Binärdateien. Dadurch sinkt deren Code-Integritätsschutz. Installieren Sie das Modul nur, wenn Sie diesen Kompromiss geprüft und akzeptiert haben. Ein Deinstallationsprogramm ist enthalten.

Datenschutz:

Keine Werbung, Analyse oder vom Projekt betriebener Server; Browserdaten werden nicht hochgeladen. Die Erweiterung kommuniziert lokal per Native Messaging. Das native Modul lädt signierte Kompatibilitätsdaten von GitHub/jsDelivr herunter, aber keine Tab-URLs, Titel oder Baumdaten hoch. Diagnoseberichte bleiben lokal, bis der Benutzer sie ausdrücklich exportiert und teilt.
