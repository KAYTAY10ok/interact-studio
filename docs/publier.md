# Préparer une release GitHub

Le workflow **Release builds** compile et teste Studio pour Windows x64 et Debian 12 amd64. Un push sur `main` vérifie la préparation des paquets. Un tag Git annoté de la forme `vMAJEURE.MINEURE.CORRECTIF` déclenche en plus la publication.

1. Alignez la version affichée par l’application avec celle de `CMakeLists.txt`. Ajoutez les notes sous `docs/releases/vX.Y.Z.md`. Adaptez les noms d’exemple dans le guide d’installation si nécessaire.
2. Vérifiez le build de la révision à publier dans l’onglet **Actions** de GitHub.
3. Créez le tag sur cette révision et envoyez-le vers GitHub.

   ```sh
   git tag -a vX.Y.Z -m 'Interact Studio X.Y.Z' <révision>
   git push origin refs/tags/vX.Y.Z
   ```

4. Vérifiez que **Publish GitHub release** a réussi. Ouvrez la page de la version et vérifiez les quatre fichiers annoncés avec le fichier `SHA256SUMS.txt`. GitHub joint aussi les archives du code source du tag.

Pour reproduire les paquets en local, utilisez `bash tools/package-linux.sh vX.Y.Z dist` après `make all check` sur Debian 12 amd64. Pour Windows, lancez `bash tools/package-windows.sh build/windows vX.Y.Z dist` après le build CMake dans le terminal MSYS2 UCRT64. L’étape Windows du workflow liste les paquets nécessaires.
