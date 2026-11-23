# Extensions Studio

Dans **Extensions → Browse extensions…** ou dans l’onglet **Extensions** des préférences, recherche un paquet, ouvre sa fiche et clique sur **Install**. Le catalogue de Studio fonctionne hors ligne. Tu peux retirer un paquet avec **Uninstall** depuis sa fiche.

Le premier catalogue contient **30 paquets** conçus pour Interact Studio, sous GPL-3.0 ou ultérieure.

- **18 commandes** pour Git, Cargo, Python, CMake, Go et Docker Compose. Une commande apparaît dans le menu **Extensions** et dans la palette. Elle utilise les outils déjà installés sur ton système et s’exécute à ta demande dans le terminal du projet.
- **8 packs d’extraits** pour Python, C++, Rust, JavaScript, TypeScript, HTML, CSS et SQL. Tape par exemple `htmldialog` dans un fichier HTML après avoir installé le pack correspondant. Accepte la suggestion avec `Tab` ou `Entrée`. La fiche du paquet indique tous ses déclencheurs.
- **4 thèmes** à sélectionner dans **Preferences → Appearance** après installation. Une feuille de style personnelle continue de s’appliquer après le thème.

Studio place les fichiers installés dans les dossiers `extensions`, `snippets` et `themes` de sa configuration utilisateur. Le catalogue conserve un fichier local si son contenu diffère du paquet. Pour personnaliser un paquet tout en conservant la possibilité de le désinstaller depuis son catalogue, crée une copie distincte de sa configuration.

Le catalogue met ses paquets à disposition dans la version de Studio qui les contient. Les paquets déclaratifs n’ajoutent pas de processus au démarrage. Les commandes demandent les outils indiqués sur leur fiche. Pour créer tes propres commandes ou modifier la feuille de style, consulte le [guide de personnalisation](customization.md).
