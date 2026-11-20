# Personnaliser Interact Studio

Ouvrez **Preferences** dans la palette de commandes. L’onglet **Appearance** règle la police, sa taille et la couleur d’accent. Cliquez sur **Save** pour appliquer ces réglages et les retrouver au prochain lancement.

Dans **Keyboard**, choisissez un profil, puis cliquez dans la cellule de la commande et tapez votre raccourci. Les raccourcis peuvent comporter plusieurs frappes. Le profil **Emacs** propose notamment `Ctrl+X` puis `Ctrl+F` pour ouvrir un fichier, `Ctrl+X` puis `Ctrl+S` pour enregistrer, `Ctrl+S` pour rechercher et `Alt+X` pour la palette. **Reset profile** restaure les raccourcis du profil sélectionné. Les personnalisations de chaque profil sont conservées séparément.

Les préférences de ce panneau sont enregistrées dans `interact-studio/settings.ini` sous le répertoire de configuration de votre compte. Sous Linux, ce répertoire est `$XDG_CONFIG_HOME`, ou `~/.config` par défaut.

## Ajouter une commande au terminal

Dans **Preferences → Extensions**, **Open folder** ouvre le dossier des manifestes de votre compte. Créez-y un fichier nommé `git-history.json` avec ce contenu.

```json
{
  "id": "git-history",
  "name": "Git · Recent commits",
  "program": "git",
  "arguments": ["log", "--oneline", "--decorate", "-15"]
}
```

Choisissez **Reload commands**. Dans un projet Git avec au moins un commit, ouvrez la palette et lancez **Git · Recent commits** pour afficher l’historique dans le terminal du projet. Le nouvel onglet apparaît lorsque vous lancez la commande. Vous pouvez lui associer une touche depuis **Keyboard**. Pour retirer la commande, supprimez le manifeste puis rechargez la liste.

Chaque fichier JSON décrit une commande. Le champ `id` est unique, commence par une lettre ASCII, un chiffre, un tiret ou un soulignement et accepte également les points aux positions suivantes (80 caractères au maximum). `name` est le libellé affiché. `program` est le nom d’un exécutable disponible ou son chemin. `arguments` est une liste facultative de chaînes, chacune transmise comme un argument distinct. L’exemple nécessite Git. Les manifestes sont lus depuis le dossier indiqué dans l’onglet **Extensions**. Cet onglet signale les doublons et les manifestes invalides. Une commande s’exécute avec les droits de votre compte lorsque vous la sélectionnez.

## Adapter l’interface avec une feuille de style

Dans **Preferences → Style sheet**, saisissez une feuille de style Qt (QSS) et cliquez sur **Save**. Elle s’applique après le thème fourni et se recharge à chaque démarrage. Cet exemple change la couleur du nom de l’application.

```css
QLabel#brand { color: #74d4bd; }
```

Vous pouvez aussi cibler `QToolBar#activityRail`, `QWidget#topbar`, `QLabel#location` et les classes Qt standard. Pour retrouver le thème fourni, effacez le contenu de cet onglet et enregistrez. Vous pouvez modifier le fichier UTF-8 `studio.qss` à côté de `settings.ini` dans votre éditeur, puis relancer Interact Studio pour le recharger.
