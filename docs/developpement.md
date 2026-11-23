# Compiler et tester Interact Studio

La compilation utilise un compilateur C++17 et Qt 5.15. Sous Linux, le terminal intégré repose aussi sur QTermWidget 5.

## Linux

Sous Debian 12, installez les dépendances de compilation.

```sh
sudo apt install build-essential pkg-config qtbase5-dev libqtermwidget5-1-dev git ripgrep
make -j2
./build/interact-studio . src/main.cpp
```

Dans le Cloud Debian 13, lancez `./tools/cloud-setup.sh` avant `make` pour préparer les en-têtes et le terminal sans droits d’administration.

```sh
make -j2 check
make bench
```

Le benchmark hors écran mesure le traitement de 500 frappes dans un fichier C++ de 5 000 lignes. Comparez des builds optimisés sur la même machine, en relevant la médiane, le 95ᵉ percentile et la mémoire résidente au repos. Mesurez séparément les serveurs de langage utilisés dans le projet.

## Windows

Ouvrez un terminal **MSYS2 UCRT64** et installez les dépendances.

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,qt5-base,ripgrep}
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --parallel 2
ctest --test-dir build/windows --output-on-failure
./build/windows/InteractStudio.exe
```

Installez aussi Git pour Windows. NSIS et `zip` sont requis pour recréer l’installateur et l’archive portable. [Préparer une release](publier.md)
