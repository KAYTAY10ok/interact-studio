# Personnaliser Interact Studio

Ouvrez **Preferences** dans la palette de commandes. L’onglet **Appearance** règle la police, sa taille, la couleur d’accent et le thème. Dans **Saving**, activez ou désactivez l’enregistrement automatique des fichiers nommés et réglez le délai après la dernière frappe. Le délai initial est de 1 500 ms. Cliquez sur **Save** pour appliquer vos réglages.

Dans **Keyboard**, choisissez un profil, puis cliquez dans la cellule de la commande et tapez votre raccourci. Les raccourcis peuvent comporter plusieurs frappes. Le profil **Emacs** propose notamment `Ctrl+X` puis `Ctrl+F` pour ouvrir un fichier, `Ctrl+X` puis `Ctrl+S` pour enregistrer, `Ctrl+S` pour rechercher et `Alt+X` pour la palette. **Reset profile** restaure les raccourcis du profil sélectionné. Les personnalisations de chaque profil sont conservées séparément.

## Modifier les préférences en JSON

L’onglet **Settings file** indique le chemin exact et ouvre `settings.json`. Sous Linux, ce fichier se trouve dans `$XDG_CONFIG_HOME/interact-studio`, ou dans `~/.config/interact-studio` par défaut. Interact Studio importe l’ancien `settings.ini` si le fichier JSON est absent et conserve l’original. Le panneau et le fichier JSON modifient les mêmes réglages.

```json
{
  "version": 1,
  "editor": {
    "fontFamily": "",
    "fontSize": 12,
    "autosave": true,
    "autosaveDelayMs": 1500
  },
  "appearance": {
    "accent": "#e0ae69",
    "theme": ""
  },
  "keyboard": {
    "profile": "Studio",
    "shortcuts": {
      "Studio": {
        "file.save": "Ctrl+S"
      }
    }
  },
  "workspaceOverrides": false
}
```

Une famille de police vide sélectionne la police monospace par défaut. Les tailles vont de 7 à 48 points, et le délai d’enregistrement de 200 à 30 000 ms. Pour un thème installé par le catalogue, utilisez son identifiant de fichier sans `.qss`. Une chaîne vide sélectionne Studio Dark. Pour désactiver un raccourci, définissez-le à `""`. Une préférence omise reprend sa valeur par défaut.

Enregistrez les modifications puis lancez **Reload settings from disk** dans la palette. Si le fichier est invalide, Studio conserve les derniers réglages valides de la session. Le panneau affiche la cause et protège le fichier jusqu’à sa correction.

Pour appliquer des réglages propres à un projet, activez **Allow workspace appearance and saving overrides** dans l’onglet **Settings file**, ou mettez `"workspaceOverrides": true` dans vos préférences. Créez ensuite `.interact-studio/settings.json` dans le projet avec les champs `editor.fontFamily`, `editor.fontSize`, `editor.autosave`, `editor.autosaveDelayMs` ou `appearance.accent` souhaités. Studio ignore les autres champs du projet. Le panneau continue d’éditer les préférences de votre compte.

## Installer une extension

Choisissez **Browse catalog…** dans **Preferences → Extensions** pour consulter les thèmes, extraits et commandes disponibles. **Install** rend l’extension disponible dans votre compte. Pour utiliser un thème installé, choisissez-le dans **Appearance**. Les extraits installés du langage courant apparaissent dans les suggestions. Les commandes installées se lancent depuis la palette ou le menu **Extensions**. Revenez au catalogue pour retirer un paquet.

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

Vous pouvez aussi cibler `QToolBar#activityRail`, `QWidget#topbar`, `QLabel#location` et les classes Qt standard. Votre feuille personnelle s’applique après le thème choisi. Pour retrouver ce thème seul, effacez le contenu de cet onglet et enregistrez. Vous pouvez modifier le fichier UTF-8 `studio.qss` à côté de `settings.json`, puis lancer **Reload settings from disk**.
