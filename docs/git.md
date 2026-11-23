# Git et GitHub dans Interact Studio

Ouvrez **Source control** pour voir la branche active et ses changements. Le panneau distingue les différences locales des différences indexées. **Stage** et **Unstage** mettent l’index à jour pour le fichier choisi. **Commit…** affiche les fichiers indexés avant de demander le message du commit.

Dans **Source control → Git → Repository identity…**, configurez votre nom et votre e-mail pour le dépôt. Si vous retirez une valeur locale, Git peut utiliser une valeur héritée de votre configuration utilisateur.

Vous pouvez changer de branche ou en créer une depuis le panneau Git. Avant un changement de branche ou un pull, enregistrez vos fichiers ouverts. Git peut refuser l’opération si elle risque d’écraser des changements du répertoire de travail.

- **Fetch** actualise les références distantes.
- **Pull** avance la branche si l’historique le permet. Une divergence exige une fusion ou un rebase décidé séparément.
- **Push** envoie les commits au dépôt distant. Si Git exige de configurer la branche distante, examinez d’abord le nom de la destination.

Dans le menu **Git**, choisissez **Open GitHub repository** pour ouvrir un dépôt GitHub dans le navigateur. Si la commande `gh` est disponible, **GitHub sign-in status** affiche l’état de la connexion et **Sign in to GitHub…** démarre la procédure GitHub. Les opérations Git utilisent également la configuration HTTPS ou SSH de votre machine.
