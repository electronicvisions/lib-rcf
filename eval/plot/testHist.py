#!/usr/bin/env python

# /usr/bin/python

import os
from matplotlib import rc
import matplotlib.pyplot as p
import numpy as n

# Filename
thisFile,thisExt= os.path.splitext(os.path.basename(__file__))

# LaTeX output
rc('text', usetex=True)
rc('font', family='serif', size=16)

# what to plot
plotColumn = 'transferDuration'

# Load data
tableType = { 'names'  : ('name', 'nr', 'transfers', 'bytesPerTransfer', 'transferDuration'),
              'formats': ('S20',  'i',  'i',         'i',                'f8') }

def loadFile(file):
    f = open(file)
    
    data = n.loadtxt(file, dtype=tableType)
    f.close()
    return data
    
fullTable = loadFile(thisFile+'.dat')

# list of names
names =  set(fullTable['name'])

p.xlabel("Time [s]")
p.ylabel("Count []")
# Adjusting figure properties
#fig = p.figure()
#ax = fig.add_subplot(111)
#ax.set_xscale('log') # adjust bins if enabled!
#mybins = [1, 10, 100, 1000, 10**4, 10**5, 10**6, 10**7, 10**8, 10**9]

for name in names:
    print "Name %s" % name
    part = [x.tolist() for x in fullTable if x[0] == name]
    data = n.array([l[1:] for l in part]) # dump 0th column :)

    data = data[:,tableType['names'].index(plotColumn)-1]

    print "Min =", n.min(data)
    print "Avg =", n.average(data)
    print "Std =", n.std(data, ddof=1)
    print "Max =", n.max(data)

    p.hist(data)#, bins=mybins, log=False)

p.savefig(thisFile+'.pdf')#, bbox_inches='tight')
p.clf()
