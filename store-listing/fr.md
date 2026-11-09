ArborTab — Onglets en arborescence dans la barre verticale native de Chrome

Windows x64 uniquement. Nécessite la barre d’onglets verticale native de Chrome. L’intégration visuelle complète requiert un module natif libre installé séparément depuis GitHub. Lisez l’avertissement de sécurité avant de l’installer.

ArborTab organise les onglets Chrome en véritable arborescence parent-enfant dans la barre verticale de Chrome, sans panneau latéral ni seconde liste d’onglets.

Fonctionnalités :

- Les onglets ouverts depuis un lien deviennent enfants de l’onglet d’origine.
- Replier et déplier des sous-arbres entiers.
- Fermer un parent et tous ses descendants.
- Glisser un onglet sur un autre pour l’imbriquer, ou sur un bord pour réordonner les frères.
- Guides d’indentation, compteur d’onglets masqués, couleurs du thème et animations facultatives.
- Raccourcis pour replier, fermer un arbre, indenter et désindenter.

L’extension seule conserve l’arbre, réordonne les onglets, gère les raccourcis et ferme les sous-arbres. Sans module natif, la barre de Chrome reste visuellement plate.

Installation du module natif :

Après l’installation de l’extension, ouvrez les paramètres d’ArborTab. Ils contiennent le lien vers la dernière version GitHub et la commande d’installation. Code source, scripts, paquets, sommes de contrôle et désinstallation :
https://github.com/EvATive7/chrome-plugin-arbortab

Commande d’installation :
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

Avertissement de sécurité :

Le composant natif est un proxy `version.dll` installé à côté de `chrome.exe`. Il s’intègre à la mise en page, au dessin, à la souris et au glisser-déposer de la barre verticale. Comme la DLL distribuée n’est pas signée par Microsoft, elle désactive pour les processus enfants de Chrome la mesure qui n’autorise que les binaires signés par Microsoft. Cela réduit leur protection d’intégrité du code. Installez-le uniquement après avoir accepté ce compromis. Le paquet comprend un programme de désinstallation.

Confidentialité :

ArborTab n’utilise ni publicité ni analyse, n’exploite aucun serveur et ne téléverse aucune donnée de navigation. L’extension communique localement par Native Messaging. Le module télécharge des données de compatibilité signées depuis GitHub/jsDelivr, sans envoyer les URL, titres ou l’arbre. Les diagnostics restent locaux sauf export et partage explicites par l’utilisateur.
