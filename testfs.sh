#!/bin/sh
# A simple and stupid script to test basic features of tarfs

TARNAME=test-tar
TRANSNODE=t
LOGFILE=test-log

# Start tarfs on TRANSNODE with the given args
function start_trans
{
  settrans -fgca $TRANSNODE ./tarfs -D $LOGFILE $*
  return $?
}

# Stop tarfs
function stop_trans
{
  [ -f $TRANSNODE ] && settrans -g $TRANSNODE || settrans -fg $TRANSNODE
  return $?
}

# A simple data consistency check
function do_diff
{
  local ret=0

  echo -n "Consistency check... "
  cd $TRANSNODE
  for i in $contents
  do
    diff "$i" ../"$i" > /dev/null || ret=1 && break
  done
  cd $homedir
  [ $ret -ne 0 ] && echo failed || echo ok

  return $ret
}

# Checks the ability to sync the filesystem
function do_rm_cp
{
  echo -n "Deleting files... "
  rm -rf ${TRANSNODE}/* && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return 1
  echo -n "Syncing... "
  stop_trans && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return 1
  echo -n "Remounting... "
  start_trans $tarfs_opts -w $tarfile && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return 1
  echo -n "Recopying everything... "
  cp -r $contents $TRANSNODE && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return 1
  echo -n "Syncing... "
  stop_trans && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return 1
  echo -n "Remounting... "
  start_trans $tarfs_opts -w $tarfile && echo "ok"
  [ $? -ne 0 ] && echo "failed" && return $?
  return 0
}

# Hello world
echo "A Small Test Suite for tarfs"
echo

# Clean up the directory and get a list of the files in here
stop_trans
rm -f $TARNAME $TARNAME.gz $TARNAME.bz2 $LOGFILE $TRANSNODE
contents=`echo *`
homedir=`pwd`

# Pack the current directory
echo -n "Building test archive ($TARNAME)... "
tar cf $TARNAME * && echo "done"
[ $? -ne 0 ] && echo "failed" && exit 1

# Launch the test suite
for tarfile in "$TARNAME" "${TARNAME}.gz" "${TARNAME}.bz2"
do
  # Get the appropriate file and tarfs options
  case "$tarfile" in
    *.gz)  tarfs_opts="-z"
           gzip $TARNAME ;;
    *.bz2) tarfs_opts="-j"
           gunzip -c $TARNAME.gz | bzip2 -c > $tarfile && rm $TARNAME.gz ;;
  esac

  echo
  echo "*** File $tarfile ***"
  start_trans $tarfs_opts "$tarfile"

  if [ $? -ne 0 ]
  then
    echo "Unable to read $tarfile"
  else
    # Read-only tests
    do_diff  || exit 1
    do_rm_cp || exit 1
    do_diff  || exit 1
  fi

  stop_trans
done
