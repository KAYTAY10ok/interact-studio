# Choix de conception

Interact Studio reprend deux idées précises des sources indiquées pour le projet.

- Dans VS Code, `registerAction2` associe une commande à son identifiant, à ses menus, à la palette et à ses raccourcis. Dans Interact Studio, une même `QAction` représente une commande accessible depuis plusieurs endroits. Modifier le raccourci d’enregistrement conserve donc le même comportement que choisir la commande dans le menu. [Code de VS Code, actions.ts](https://github.com/microsoft/vscode/blob/1934a004211cdeb23d9caa8062379e55d0689368/src/vs/platform/actions/common/actions.ts#L821)
- Emacsmirror regroupe de nombreux dépôts de paquets Emacs. Dans le paquet Consult, `consult-buffer-sources` permet de définir les sources offertes par une seule commande de sélection. Interact Studio applique cette idée à sa palette, où les commandes de l’application et les commandes ajoutées par l’utilisateur sont accessibles au clavier. [Code de Consult, consult.el](https://github.com/emacsmirror/consult/blob/a64569f377b8bc0f62e1d2ae0f3ee48ba4a7cc1c/consult.el#L239)

L’application est une implémentation en C++ et Qt Widgets. Le dépôt de VS Code est sous licence MIT et Consult est sous GPL-3.0-or-later. Ces références ont servi à étudier le fonctionnement des commandes. Leurs sources, logos et moteurs d’extensions ne sont pas distribués dans Interact Studio. [Licence de VS Code](https://github.com/microsoft/vscode/blob/1934a004211cdeb23d9caa8062379e55d0689368/LICENSE.txt) · [Licence de Consult](https://github.com/emacsmirror/consult/blob/a64569f377b8bc0f62e1d2ae0f3ee48ba4a7cc1c/consult.el#L15)

L’interface utilise Qt 5. Le terminal utilise QTermWidget 5, lié dynamiquement. Le paquet Debian QTermWidget 5 est distribué sous GPL-2.0 ou ultérieure, avec des licences propres à certains fichiers. Ces dépendances sont installées à part. [Qt](https://www.qt.io/licensing/) · [QTermWidget](https://github.com/lxqt/qtermwidget)
