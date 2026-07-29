#!/usr/bin/env bash
# ============================================================
# Publie une version du firmware veilleuse sur GitHub Releases.
# Les lampes du parc verifient "latest" chaque nuit vers 4 h et se
# mettent a jour toutes seules -> une release cassee brique le parc
# (l'ESP8266 n'a pas de partition de secours). D'ou l'etape canari.
#
# Prerequis : pio, gh authentifie, arbre git propre et pousse.
# Usage : ./scripts/release.sh
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

MAIN=firmware/src/main.cpp
BIN=firmware/.pio/build/d1_mini/firmware.bin
SECRETS=firmware/include/secrets.h
TMP=$(mktemp -d)
DEVSECRETS="$TMP/secrets.h.dev"
HAD_SECRETS=0

# Le secrets.h du poste de dev doit TOUJOURS etre remis en place, meme si le
# script echoue ou est interrompu.
restore_secrets() { [[ "$HAD_SECRETS" == 1 ]] && cp "$DEVSECRETS" "$SECRETS"; }
cleanup() { restore_secrets; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# --- 1. Arbre propre et synchronise avec origin/main ---------------------
if [[ -n "$(git status --porcelain)" ]]; then
  echo "ERREUR : arbre git non propre (commit ou stash d'abord)." >&2
  exit 1
fi
git fetch origin main --quiet
if [[ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]]; then
  echo "ERREUR : HEAD != origin/main (git push d'abord)." >&2
  exit 1
fi

# --- 2. Version : source de verite = FW_VERSION dans main.cpp ------------
VER=$(grep -oE '#define[[:space:]]+FW_VERSION[[:space:]]+"[0-9]+\.[0-9]+\.[0-9]+"' "$MAIN" \
      | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
if [[ -z "${VER:-}" ]]; then
  echo "ERREUR : FW_VERSION introuvable ou mal formee dans $MAIN." >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/v$VER" >/dev/null; then
  echo "ERREUR : le tag v$VER existe deja (incrementer FW_VERSION ?)." >&2
  exit 1
fi

# --- 3. Build SANS identifiants ------------------------------------------
# Un binaire publie ne doit contenir AUCUN identifiant : il est telechargeable
# par n'importe qui et `strings` suffit a les extraire. Le secrets.h du poste
# de dev est donc mis de cote pendant la compilation.
if [[ -f "$SECRETS" ]]; then
  cp "$SECRETS" "$DEVSECRETS"
  HAD_SECRETS=1
fi
cat > "$SECRETS" <<'EOS'
// Genere par scripts/release.sh — un binaire publie ne contient aucun
// identifiant : la lampe se configure par son portail captif.
#pragma once
#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""
#define DEXCOM_USERNAME ""
#define DEXCOM_PASSWORD ""
#define OTA_PASSWORD    "veilleuse"
EOS

echo "== Build v$VER (sans identifiants) =="
( cd firmware && pio run -e d1_mini )
[[ -f "$BIN" ]] || { echo "ERREUR : $BIN absent apres le build." >&2; exit 1; }

# --- 3b. Garde-fou : verifier que rien n'a fuite dans le binaire ----------
# Le seul filet qui compte : on cherche litteralement les valeurs du secrets.h
# de dev dans le binaire compile. Si l'une y est, on refuse de publier.
if [[ "$HAD_SECRETS" == 1 ]]; then
  for k in WIFI_SSID WIFI_PASSWORD DEXCOM_USERNAME DEXCOM_PASSWORD; do
    val=$(grep -oE "$k[[:space:]]+\"[^\"]*\"" "$DEVSECRETS" | sed -E 's/.*"(.*)"/\1/' || true)
    [[ ${#val} -lt 4 ]] && continue
    if strings "$BIN" | grep -qF -- "$val"; then
      echo "ERREUR : le binaire contient la valeur de $k de votre secrets.h." >&2
      echo "         Publication annulee (fuite d'identifiants evitee)." >&2
      exit 1
    fi
  done
  echo "   garde-fou : aucun identifiant du poste de dev dans le binaire."
fi

cp "$BIN" "$TMP/firmware.bin"
printf '%s\n' "$VER" > "$TMP/version.txt"

# --- 4. Canari : valider sur SA lampe avant d'exposer le parc ------------
cat <<EOF

=========================== CANARI ===========================
Avant de publier v$VER a TOUTES les lampes :

  1. Flasher votre lampe en OTA local :
       cd firmware && pio run -e d1_mini_ota -t upload \\
            --upload-port veilleuse-XXXXXX.local
     (le hostname s'affiche en serie au boot : "[id] ... hostname ...")

  2. Verifier : boot OK, glycemie correcte, couleurs normales,
     portail accessible. Laisser tourner un moment.

Rappel : pas de retour arriere automatique sur ESP8266.
==============================================================

EOF
read -r -p "Canari valide -> publier la release v$VER ? [y/N] " ok
[[ "${ok:-}" == "y" || "${ok:-}" == "Y" ]] || { echo "Annule."; exit 1; }

# --- 5. Publication (cree aussi le tag v$VER) ----------------------------
gh release create "v$VER" "$TMP/firmware.bin" "$TMP/version.txt" \
   --title "v$VER" --generate-notes

echo
echo "OK : v$VER publiee. Les lampes la prendront a leur check de 4 h."
