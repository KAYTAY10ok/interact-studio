# Éditer et prévisualiser

Studio détecte le langage d’après le nom du fichier. La barre d’état indique le mode actif. Les fichiers de type inconnu s’ouvrent en texte simple.

Les modes couvrent notamment C/C++, Rust, Python, JavaScript et TypeScript avec JSX/TSX, HTML, Vue, Svelte, CSS, JSON, YAML, TOML, Markdown, Lua, PHP, Java, Kotlin, C#, Go, Ruby, Swift, Shell, PowerShell, SQL, XML, Dockerfile et Makefile.

## Suggestions et indentation

Commencez à taper un mot dans l’éditeur ou appuyez sur `Ctrl+Espace` pour demander des suggestions. Choisissez avec les flèches, puis validez avec `Tab` ou `Entrée`. `Échap` ferme la liste. Les suggestions utilisent les mots du document, les mots-clés du langage et les extraits intégrés. La sélection de types et de membres à partir d’un projet entier nécessite un moteur d’analyse sémantique.

`Entrée` reprend l’indentation de la ligne. Studio l’adapte au contexte reconnu, par exemple après une accolade ouvrante ou un deux-points Python. La complétion et l’indentation fonctionnent aussi dans la vue scindée.

## Fichiers et dossiers

Le clic droit dans **Explorer** propose les actions adaptées à l’élément sélectionné. Pour créer un fichier à côté d’un fichier existant, faites un clic droit sur le dossier parent ou sur le fichier. **Rename** met à jour le chemin des onglets concernés. **Move to Trash** demande confirmation et utilise la corbeille du système. Enregistrez ou fermez les documents modifiés avant de renommer ou de retirer leurs fichiers.

## Live Server

1. Ouvrez un projet web statique et choisissez une page HTML. Pour lancer la page d’accueil d’un dossier, celui-ci doit contenir un fichier `index.html`.
2. Cliquez sur **Go Live** dans la barre d’état, ou faites un clic droit et choisissez **Open with Live Server**. La page s’ouvre à une adresse locale dans votre navigateur.
3. Modifiez puis enregistrez les fichiers du site. La page ouverte se recharge automatiquement après une modification sur disque.
4. Cliquez sur **Stop Live** pour arrêter le serveur.

La prévisualisation sert les fichiers statiques du projet sur une adresse accessible uniquement depuis votre machine. Pour exécuter un serveur d’application ou un outil comme Vite, utilisez **Run workspace task…** avec la commande du projet.
