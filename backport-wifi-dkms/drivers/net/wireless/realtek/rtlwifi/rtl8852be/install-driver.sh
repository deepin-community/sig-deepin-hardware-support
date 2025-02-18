#!/bin/bash

SCRIPT_NAME="install-driver.sh"
SCRIPT_VERSION="20220223"

DRV_NAME="rtl8852be"
DRV_VERSION="1.15.10.0.2"

KRNL_VERSION="$(uname -r)"

NO_PROMPT=0

clear
echo "Running ${SCRIPT_NAME} version ${SCRIPT_VERSION}"

# Get the options
while [ $# -gt 0 ]
do
	case $1 in
		NoPrompt)
			NO_PROMPT=1 ;;
		*h|*help|*)
			echo "Syntax $0 <NoPrompt>"
			echo "       NoPrompt - noninteractive mode"
			echo "       -h|--help - Show help"
			exit 1
			;;
	esac
	shift
done

# check to ensure sudo was used
if [[ $EUID -ne 0 ]]
then
	echo "You must run this script with superuser (root) privileges."
	echo "Try: \"sudo ./${SCRIPT_NAME}\""
	exit 1
fi

# check for previous installation
if [[ -d "/usr/src/${DRV_NAME}-${DRV_VERSION}" ]]
then
	/usr/src/backport-wifi-1.2.3/realtek/rtl8852be/remove-driver.sh
fi

echo "Starting installation..."
# the add command requires source in /usr/src/${DRV_NAME}-${DRV_VERSION}
echo "Copying source files to: /usr/src/${DRV_NAME}-${DRV_VERSION}"
cp -rf /usr/src/backport-wifi-1.2.3/realtek/rtl8852be /usr/src/${DRV_NAME}-${DRV_VERSION}

dkms add -m ${DRV_NAME} -v ${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]
then
	echo "An error occurred. dkms add error = ${RESULT}"
	echo "Please report this error."
	exit $RESULT
fi

dkms build -m ${DRV_NAME} -v ${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]
then
	echo "An error occurred. dkms build error = ${RESULT}"
	echo "Please report this error."
	exit $RESULT
fi

dkms install -m ${DRV_NAME} -v ${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]
then
	echo "An error occurred. dkms install error = ${RESULT}"
	echo "Please report this error."
	exit $RESULT
fi

echo "The driver was installed successfully."

exit 0
