# NEVER commit these — read before your first push

This is a **public** repository. Git history is permanent: anything committed
once stays retrievable forever, even after it is deleted. The rule is
prevention, not cleanup.

## 1. Real databases
Never commit `spData.sqlite` or any real database — not even "temporarily as
test data." A real database contains the church's entire song library, Bible
modules, announcements, and usage history. For testing, use the synthetic
fixture (a handful of fake songs and a public-domain psalm) — ask before
inventing a new one.

## 2. Server URLs with credentials, tokens, API keys, passwords
Sync/server configuration lives in a local config file that is `.gitignore`d.
If you are typing a secret into a file that git tracks, stop.

## 3. Screenshots with real content
A screenshot of the announcements panel can expose personal prayer requests;
a schedule screenshot exposes real service content. Use demo data for anything
visual (README, issues, help pages).

## 4. Bible modules
NKJV, Schlachter 2000 and similar translations are copyrighted. Do not commit
Bible module files or exports of them.

## 5. CGF branding
The CGF logo and branding assets are not covered by the GPL license of this
code and are kept out of this repository.

## If something slips through
Do not just delete the file in a new commit — that leaves it in history.
Tell the maintainer immediately; the remedy is a history rewrite plus rotating
any exposed secret, and it is much cheaper the sooner it happens.
