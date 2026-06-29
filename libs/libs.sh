#!/bin/bash
cd "$(dirname "$0")"
set -eu

# Braid dependency installer
# Downloads only the libraries Braid needs from ofWorks/ofLibs.
# Dawn is fetched from tag "add"; everything else uses v1.0.

LIBSVERSION=v1.0
DAWN_TAG=add
BRAID_LIBS_REPO="ofWorks/ofLibs"
BRAID_LIBS_COMMIT_FILE=".braid_libs"
WGET2VERSION=v2.2.1

# Function to get the latest Chalet version from GitHub
get_latest_chalet_version() {
	local latest_version=""
	if command -v curl &>/dev/null; then
		latest_version=$(curl -s "https://api.github.com/repos/chalet-org/chalet/releases/latest" 2>/dev/null | grep -o '"tag_name": "[^"]*"' | head -1 | cut -d'"' -f4 | sed 's/^v//')
	fi
	echo "${latest_version:-0.8.18}"
}

CHALETVERSION=$(get_latest_chalet_version)

wipeDownloads=true
wipeDownloadsAfterInstall=true
wipeLibs=true

COLOR='\033[0;32m'
COLOR2='\033[0;34m'
NC='\033[0m'

section() {
	printf "${COLOR}%s${NC}\n" "$*"
}

sectionOk() {
	printf "[${COLOR}✓${NC}] ${COLOR}%s${NC}\n" "$*"
}

executa() {
	printf "${COLOR2}%s${NC}\n" "$*"
	"$@"
}

alert() {
	printf "⚠️ ${COLOR2}%s${NC}\n" "$*"
}

section "💾 Installing Braid libraries"

# ============================================
# Check Latest Commit from ofWorks/ofLibs
# ============================================

gh_is_authenticated() {
	if command -v gh &>/dev/null; then
		gh auth status &>/dev/null 2>&1
		return $?
	fi
	return 1
}

get_remote_commit() {
	local commit_hash=""
	if gh_is_authenticated; then
		commit_hash=$(gh api repos/${BRAID_LIBS_REPO}/commits/main --jq '.sha' 2>/dev/null || true)
	fi
	if [[ -z "$commit_hash" ]]; then
		commit_hash=$(curl -s "https://api.github.com/repos/${BRAID_LIBS_REPO}/commits/main" 2>/dev/null | grep -o '"sha": "[^"]*"' | head -1 | cut -d'"' -f4)
	fi
	if [[ -n "$commit_hash" ]]; then
		echo "${commit_hash:0:7}"
	fi
}

STORED_COMMIT=""
if [[ -f "$BRAID_LIBS_COMMIT_FILE" ]]; then
	STORED_COMMIT=$(cat "$BRAID_LIBS_COMMIT_FILE" 2>/dev/null || true)
	sectionOk "Stored commit: ${STORED_COMMIT:-none}"
fi

INSTALLED_CHALET_VERSION=""
if command -v chalet &>/dev/null; then
	INSTALLED_CHALET_VERSION=$(chalet --version 2>/dev/null | awk '{print $3}')
fi

section "Checking latest commit from ${BRAID_LIBS_REPO}..."
REMOTE_COMMIT=$(get_remote_commit)

if [[ -z "$REMOTE_COMMIT" ]]; then
	alert "Could not fetch latest commit from remote. Proceeding with installation..."
else
	sectionOk "Remote commit: $REMOTE_COMMIT"

	LIBS_UP_TO_DATE=false
	CHALET_UP_TO_DATE=false

	if [[ -n "$STORED_COMMIT" && "$STORED_COMMIT" == "$REMOTE_COMMIT" ]]; then
		LIBS_UP_TO_DATE=true
	fi

	if [[ -n "$INSTALLED_CHALET_VERSION" && "$INSTALLED_CHALET_VERSION" == "$CHALETVERSION" ]]; then
		CHALET_UP_TO_DATE=true
	fi

	if [[ "$LIBS_UP_TO_DATE" == true && "$CHALET_UP_TO_DATE" == true ]]; then
		sectionOk "Libraries are up-to-date (commit: $REMOTE_COMMIT)"
		sectionOk "Chalet is up-to-date (version: $CHALETVERSION)"
		sectionOk "Skipping installation. Delete $BRAID_LIBS_COMMIT_FILE to force reinstall."
		exit 0
	fi

	if [[ "$LIBS_UP_TO_DATE" == false && -n "$STORED_COMMIT" ]]; then
		section "New commit detected: $STORED_COMMIT -> $REMOTE_COMMIT"
	fi

	if [[ "$CHALET_UP_TO_DATE" == false && -n "$INSTALLED_CHALET_VERSION" ]]; then
		section "New Chalet version detected: $INSTALLED_CHALET_VERSION -> $CHALETVERSION"
	fi
fi

# ============================================
# Platform Detection
# ============================================
PLATFORM="${PLATFORM:-}"
while [[ $# -gt 0 ]]; do
	case $1 in
	-p=*|--platform=*)
		PLATFORM="${1#*=}"
		shift
		;;
	-p|--platform)
		PLATFORM="$2"
		shift 2
		;;
	*)
		echo "Unknown option: $1"
		exit 1
		;;
	esac
done

if [[ -n "$PLATFORM" ]]; then
	section "Using manually specified platform: $PLATFORM"
else
	if [[ "$OSTYPE" == "darwin"* ]]; then
		PLATFORM=macos
	else
		echo "Unsupported OS: $OSTYPE (Braid is macOS-only)"
		exit 1
	fi
	section "Platform: $PLATFORM"
fi

# ============================================
# Braid Library List
# ============================================
# Core: dawn (WebGPU), fmt, glm
# Image codec stack: mango bundles its own transitive deps (deflate, lcms2, zstd, bz2, lz4)
BRAIDLIBS=( dawn fmt glm mango zlib-ng )

# ============================================
# Setup Directories
# ============================================
LIBS_FOLDER="./${PLATFORM}"

if [[ "$wipeLibs" == true && -d "${LIBS_FOLDER}" ]]; then
	executa rm -rf "${LIBS_FOLDER}"
fi

DOWNLOAD="./_braidLibs_${LIBSVERSION}_${PLATFORM}"

if [[ "$wipeDownloads" == true && -d "${DOWNLOAD}" ]]; then
	echo "Removing previously downloaded libraries"
	rm -rf "${DOWNLOAD}"
fi

echo "Creating download folder ${DOWNLOAD}"
mkdir -p "${DOWNLOAD}"

# ============================================
# macOS Tooling (chalet, wget2, ninja)
# ============================================
case "$PLATFORM" in
	macos)
		if [[ -z "${CI:-}" ]]; then
			if command -v brew &> /dev/null; then
				if command -v chalet &> /dev/null; then
					version=$(chalet --version | awk '{print $3}')
					if [ "$CHALETVERSION" != "$version" ]; then
						brew uninstall chalet
					fi
				fi

				if ! command -v wget2 &> /dev/null; then
					brew install wget2
				else
					sectionOk "wget2 already installed"
				fi

				if ! command -v chalet &> /dev/null; then
					brew tap chalet-org/chalet
					brew install --cask chalet
				else
					sectionOk "chalet already installed"
				fi

				if ! command -v ninja &> /dev/null; then
					brew install ninja
				else
					sectionOk "ninja already installed"
				fi
			else
				alert "Homebrew not installed — skipping tool installation"
			fi
		fi
		;;
	*)
		echo "Unknown platform: $PLATFORM"
		exit 1
		;;
esac

# ============================================
# Download Helpers
# ============================================

getlink() {
	if gh_is_authenticated; then
		section "Downloading with GH (github command line)"
		for libname in "${BRAIDLIBS[@]}"; do
			local tag="$LIBSVERSION"
			[[ "$libname" == "dawn" ]] && tag="$DAWN_TAG"
			gh release download "${tag}" -R "${BRAID_LIBS_REPO}" \
				--pattern "ofLibs_${libname}_${PLATFORM}.zip" -D "${DOWNLOAD}" || true
		done
	elif command -v wget2 &>/dev/null; then
		section "Downloading with wget2 (parallel)"
		PARAMS=""
		for libname in "${BRAIDLIBS[@]}"; do
			local tag="$LIBSVERSION"
			[[ "$libname" == "dawn" ]] && tag="$DAWN_TAG"
			PARAMS+=" https://github.com/${BRAID_LIBS_REPO}/releases/download/${tag}/ofLibs_${libname}_${PLATFORM}.zip"
		done
		set +e
		wget2 ${PARAMS} -P "${DOWNLOAD}"
		WGET_EXIT=$?
		set -e
		if [[ $WGET_EXIT -ne 0 ]]; then
			alert "wget2 returned exit code: $WGET_EXIT (may be normal if files are up-to-date)"
		fi
	elif command -v wget &>/dev/null; then
		section "Downloading with wget (sequential)"
		for libname in "${BRAIDLIBS[@]}"; do
			local tag="$LIBSVERSION"
			[[ "$libname" == "dawn" ]] && tag="$DAWN_TAG"
			local filepath="${DOWNLOAD}/ofLibs_${libname}_${PLATFORM}.zip"
			executa wget -N --no-verbose --show-progress \
				"https://github.com/${BRAID_LIBS_REPO}/releases/download/${tag}/ofLibs_${libname}_${PLATFORM}.zip" \
				-P "${DOWNLOAD}"
		done
	else
		section "Downloading with curl (sequential)"
		for libname in "${BRAIDLIBS[@]}"; do
			local tag="$LIBSVERSION"
			[[ "$libname" == "dawn" ]] && tag="$DAWN_TAG"
			local filepath="${DOWNLOAD}/ofLibs_${libname}_${PLATFORM}.zip"
			if [[ -f "${filepath}" ]]; then
				executa curl -L -z "${filepath}" -o "${filepath}" \
					"https://github.com/${BRAID_LIBS_REPO}/releases/download/${tag}/ofLibs_${libname}_${PLATFORM}.zip"
			else
				executa curl -L -o "${filepath}" \
					"https://github.com/${BRAID_LIBS_REPO}/releases/download/${tag}/ofLibs_${libname}_${PLATFORM}.zip"
			fi
		done
	fi
}

unzipLibs() {
	section "Uncompressing Braid libraries"

	mkdir -p "${LIBS_FOLDER}/_licenses"

	for libname in "${BRAIDLIBS[@]}"; do
		local filename="${DOWNLOAD}/ofLibs_${libname}_${PLATFORM}.zip"
		if [[ ! -f "${filename}" ]]; then
			alert "Missing archive: ${filename} — skipping"
			continue
		fi
		executa unzip -qq -o "${filename}" -d "${LIBS_FOLDER}"

		# Move license files to _licenses folder
		for file in \
			"${LIBS_FOLDER}"/*.{txt,md,mit} \
			"${LIBS_FOLDER}"/*.{TXT,MD,MIT} \
			"${LIBS_FOLDER}"/{license,License,LICENSE,licence,Licence,LICENCE} \
			"${LIBS_FOLDER}"/{licenses,Licenses,LICENSES,licences,Licences,LICENCES} \
			"${LIBS_FOLDER}"/{copying,Copying,COPYING} \
			"${LIBS_FOLDER}"/{copying,Copying,COPYING}.* \
			"${LIBS_FOLDER}"/{notice,Notice,NOTICE} \
			"${LIBS_FOLDER}"/{notice,Notice,NOTICE}.*; do
			if [[ -f "$file" ]]; then
				local basename=$(basename "$file")
				mv "$file" "${LIBS_FOLDER}/_licenses/${libname}_${basename}"
			fi
		done
		if [[ -d "${LIBS_FOLDER}/LICENSES" ]]; then
			mv "${LIBS_FOLDER}/LICENSES" "${LIBS_FOLDER}/_licenses/${libname}_LICENSES"
		fi
	done
}

# ============================================
# Main Execution
# ============================================

executa mkdir -p "${DOWNLOAD}"
getlink
unzipLibs

if [[ "$wipeDownloadsAfterInstall" == true && -d "${DOWNLOAD}" ]]; then
	echo "Removing downloaded libraries"
	rm -rf "${DOWNLOAD}"
fi

if [[ -n "$REMOTE_COMMIT" ]]; then
	echo "$REMOTE_COMMIT" > "$BRAID_LIBS_COMMIT_FILE"
	sectionOk "Saved commit hash: $REMOTE_COMMIT"
fi

sectionOk "Braid libraries installed"

trap 'printf "${NC}"' EXIT
