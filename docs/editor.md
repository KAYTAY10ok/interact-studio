# Éditer et prévisualiser

Studio détecte le langage d’après le nom du fichier. La barre d’état indique le mode actif. Les fichiers de type inconnu s’ouvrent en texte simple.

Les modes couvrent notamment C/C++, Rust, Python, JavaScript et TypeScript avec JSX/TSX, HTML, Vue, Svelte, CSS, JSON, YAML, TOML, Markdown, Lua, PHP, Java, Kotlin, C#, Go, Ruby, Swift, Shell, PowerShell, SQL, XML, Dockerfile et Makefile.

## Suggestions et indentation

Commencez à taper un mot dans l’éditeur ou appuyez sur `Ctrl+Espace` pour demander des suggestions. Choisissez avec les flèches, puis validez avec `Tab` ou `Entrée`. `Échap` ferme la liste. Les suggestions locales utilisent les mots du document, les mots-clés du langage et les extraits intégrés.

`Entrée` reprend l’indentation de la ligne. Studio l’adapte au contexte reconnu, par exemple après une accolade ouvrante ou un deux-points Python. La complétion et l’indentation fonctionnent aussi dans la vue scindée.

## Suggestions sémantiques et diagnostics

Installez `clangd` pour C/C++ ou `rust-analyzer` pour Rust, puis ajoutez le serveur au `PATH`. Les suggestions sémantiques, les signatures et les diagnostics deviennent disponibles selon les capacités du serveur et la configuration du projet. Pour C/C++, un fichier `compile_commands.json` aide le serveur à utiliser les bonnes options de compilation.

Utilisez `Ctrl+Maj+Espace` pour afficher une signature, `Ctrl+Maj+M` pour ouvrir **Problems** et `F12` pour aller à la définition. Double-cliquez sur un diagnostic pour aller à sa ligne. Des suggestions locales restent disponibles sans serveur de langage.

## Enregistrement et récupération

Dans **Preferences → Saving**, choisissez le délai ou désactivez l’enregistrement automatique. L’auto-save écrit les fichiers nommés après une pause. Si le contenu a changé sur disque depuis son ouverture, Studio protège la version externe et garde le brouillon.

Après une interruption, ouvrez de nouveau le projet puis choisissez **File → Restore drafts…**. Les brouillons sont locaux et peuvent inclure des onglets sans nom. Relisez le brouillon avant de l’enregistrer.

## Formatage et tests

**Edit → Format document** (`Alt+Maj+F`) envoie le texte au formateur adapté. Installez l’outil voulu sur le `PATH`, par exemple `clang-format`, `rustfmt`, `gofmt`, `black`, `shfmt` ou `prettier`. Le résultat s’applique dans l’éditeur et peut être annulé.

**Run → Run project tests…** propose une commande adaptée aux fichiers du projet, par exemple `cargo test` ou `npm test`. Vérifiez la commande avant de la lancer. Le résultat s’affiche dans la console.

## Fichiers et dossiers

Le clic droit dans **Explorer** propose les actions adaptées à l’élément sélectionné. Pour créer un fichier à côté d’un fichier existant, faites un clic droit sur le dossier parent ou sur le fichier. **Rename** met à jour le chemin des onglets concernés. **Move to Trash** demande confirmation et utilise la corbeille du système. Enregistrez ou fermez les documents modifiés avant de renommer ou de retirer leurs fichiers.

## Live Server

1. Ouvrez un projet web statique et choisissez une page HTML. Pour lancer la page d’accueil d’un dossier, celui-ci doit contenir un fichier `index.html`.
2. Cliquez sur **Go Live** dans la barre d’état, ou faites un clic droit et choisissez **Open with Live Server**. La page s’ouvre à une adresse locale dans votre navigateur.
3. Modifiez puis enregistrez les fichiers du site. La page ouverte se recharge automatiquement après une modification sur disque.
4. Cliquez sur **Stop Live** pour arrêter le serveur.

La prévisualisation sert les fichiers statiques du projet sur une adresse accessible uniquement depuis votre machine. Pour exécuter un serveur d’application ou un outil comme Vite, utilisez **Run workspace task…** avec la commande du projet.
