# Installer Interact Studio

Téléchargez les fichiers correspondant à votre système depuis les [releases GitHub](https://github.com/KAYTAY10ok/interact-studio/releases). Chaque release contient un fichier `SHA256SUMS.txt` pour contrôler les téléchargements.

## Windows 10 ou 11, x64

Choisissez `InteractStudio-v0.1.0-windows-x86_64-setup.exe` pour installer Studio dans votre compte. Le ZIP portable contient également `InteractStudio.exe`. Extrayez tout le ZIP et lancez l’exécutable depuis le dossier extrait, avec ses DLL et le dossier `platforms`.

L’installeur de cette version est fourni sans signature Authenticode. Windows peut afficher un avertissement d’éditeur inconnu. Vérifiez la provenance et la somme SHA-256 avant de lancer l’installeur.

Pour calculer la somme SHA-256 d’un fichier téléchargé, ouvrez PowerShell dans son dossier et comparez la valeur produite à celle de `SHA256SUMS.txt`.

```powershell
Get-FileHash .\InteractStudio-v0.1.0-windows-x86_64-setup.exe -Algorithm SHA256
```

Dans Studio, utilisez **File → Open workspace…** pour choisir un projet. Installez [Git pour Windows](https://git-scm.com/download/win) pour les fonctions Git et [ripgrep](https://github.com/BurntSushi/ripgrep/releases) pour la recherche et l’ouverture rapide par `Ctrl+P`. Relancez Studio après l’ajout de ces outils au `PATH`.

Les compilateurs, serveurs de langage, Docker, GitHub CLI, SSH et WSL sont aussi découverts via le `PATH`. Les tâches et extensions affichent leurs sorties dans Studio. Les shells interactifs, dont WSL et SSH, s’ouvrent dans une console Windows distincte.

Pour retirer l’installation, utilisez **Paramètres → Applications → Applications installées → Interact Studio → Désinstaller**. Le ZIP portable se retire en supprimant son dossier extrait.

## Debian 12, amd64

Pour installer le paquet système et ses dépendances, téléchargez `interact-studio_0.1.0_amd64.deb`, puis exécutez ces commandes dans son dossier. Studio est ensuite accessible depuis le menu des applications ou via `interact-studio`.

```sh
sudo apt install ./interact-studio_0.1.0_amd64.deb
interact-studio /chemin/vers/le/projet
```

Pour un lancement sans installation du programme, installez les dépendances système, puis extrayez le `.tar.gz`.

```sh
sudo apt install libqt5widgets5 libqtermwidget5-1 qtermwidget5-data git ripgrep
tar -xzf interact-studio-v0.1.0-linux-x86_64.tar.gz
./interact-studio-v0.1.0-linux-x86_64/interact-studio .
```

Comparez le résultat de `sha256sum <fichier>` à `SHA256SUMS.txt`. Sur l’image Codex Cloud Debian 13, utilisez la compilation depuis les sources et `tools/cloud-setup.sh` pour obtenir QTermWidget 5.

Pour adapter le thème, les raccourcis ou les menus, consultez le [guide de personnalisation](customization.md).
