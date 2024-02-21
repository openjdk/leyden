get_tput_results() {
  local tput_file=$1
  if [ ! -f ${tput_file} ]; then
    error "jmeter throughput file ${tput_file} not found."
    exit 1
  fi
  workdir=`mktemp -d`
  awk '/summary \+/' ${tput_file} > ${workdir}/tputlines
  awk '{ print $5 }' ${workdir}/tputlines > ${workdir}/time
  awk -F ":" 'BEGIN { total=0 } { total += $3; print total }' ${workdir}/time > ${workdir}/time.tmp
  awk '{ print $7 }' ${workdir}/tputlines | cut -d '/' -f 1 > ${workdir}/rampup

  echo "time,tput" > ${tput_file}.rampup.csv
  paste -d "," ${workdir}/time.tmp ${workdir}/rampup >> ${tput_file}.rampup.csv
  rm -fr ${workdir}
}

for file in `ls tput-*.log`; do
  get_tput_results ${file}
done


