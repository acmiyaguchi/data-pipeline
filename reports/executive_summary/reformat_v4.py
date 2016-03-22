'''
reformat_v4.py
--------------

This script automatically formats any of the v4 rollups generated by Trink
into more consumable dashboard-friendly formats. It simplifies some fields
and creates 'all' categories for the facets so the js on the frontend has to
do only a little bit of processing before everything hits crossfilter.


'''

import csv
import argparse
import datetime

parser = argparse.ArgumentParser(description='Reformats the v4 data')
parser.add_argument('-f', '--file', type=str, help='input file to be converted')
parser.add_argument('-o', '--output', type=str, help='output file')
args = parser.parse_args()

INPUT  = args.file
OUTPUT = args.output


f = open(INPUT, 'r')
r = csv.reader(f)

headers = r.next()
COUNTRIES = set(['US','CA','BR','MX','FR','ES','IT','PL','TR','RU','DE','IN','ID','CN','JP','GB'])
OSES = {'WINNT': 'Windows', "Darwin": "Mac", "Linux": "Linux", 'Other':'Other'}
CHANNELS = set(['release', 'beta', 'aurora', 'nightly'])
data_keys = ['actives', 'hours','inactives','new_records', 'five_of_seven',  'total_records', 'crashes', 'default','google', 'bing',  'yahoo',  'other'];
out={}

def num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)
total=0

for line in r:

    line = dict(zip(headers,line))
    if line['date'] < datetime.datetime.now().strftime('%Y-%m-%d'):
        # Don't re-aggregate 'all' lines
        if line['geo'] == 'all' or line['channel'] == 'all' or line['os'] == 'all':
            continue

        if line['geo'] not in COUNTRIES: line['geo']='Other'
        if line['channel'] not in CHANNELS: line['channel'] = 'Other'
        for geo in ['all', line['geo']]:
            if geo not in out: out[geo]={}
            for channel in ['all', line['channel']]:
                if channel not in out[geo]: out[geo][channel]={}
                for os in ['all', line['os']]:
                    if os not in out[geo][channel]: out[geo][channel][os]={}
                    dt = line['date']
                    if dt not in out[geo][channel][os]: out[geo][channel][os][dt]={}
                    for d in data_keys:
                        if d not in out[geo][channel][os][dt]: out[geo][channel][os][dt][d]=0
                        out[geo][channel][os][dt][d]+=num(line[d])

w = csv.writer(open(OUTPUT, 'w'))
w.writerow(headers)

for g in out:
    for c in out[g]:
        for o in out[g][c]:
            for dt in out[g][c][o]:
                data_values = [out[g][c][o][dt][_] for _ in data_keys]
                w.writerow([g,c,o,dt] + data_values)