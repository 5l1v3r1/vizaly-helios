# Gnuplot script for density guided compression

reset
set terminal postscript eps enhanced color 14 size 16cm, 9cm
set output "spectrum_decomp.eps"

set multiplot layout 1,2 title 'power spectrum - timestep 499 - 1073726359 particles'

array colors[7];
colors[1] = "#FF00FF"
colors[2] = "#006400"
colors[3] = "#CB0707"
colors[4] = "#0000FF"
colors[5] = "#800080"
colors[6] = "#FF00FF"
colors[7] = "#A0522D"


# ---------------
set title 'power spectrum'
set size ratio 1
set xlabel "k (h/Mpc)"
set ylabel "P_0(k) (Mpc/h)^3"
set key Left reverse below maxcols 2
set grid
set logscale x 10
set logscale y 10
set xrange [0.01:10]
set format y "10^{%2T}"

plot 'pk-orig.dat' using 1:2:3 title "original" w errorbars lc rgb "#000000",\
     'pk-decomp.dat' using 1:2:3 title "compressed" w errorbars lc rgb colors[1],\

# ---------------
reset
set title 'discrepancy'
set xlabel "wavenumber k (h/Mpc)"
set size ratio 0.9
set ylabel "P_{noised} / P_{orig}"
set key Left reverse below maxcols 2

set grid
unset logscale

set xrange [0:10]
#set logscale x 10
# set yrange [0.99:1.001]

plot 'ratio.txt' using 1:($2/$2) title "original" w lines lc rgb "#000000",\
     'ratio.txt' using 1:($3/$2) title "compressed" w lines lc rgb colors[1]

unset multiplot