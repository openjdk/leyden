/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 Used by DemoSupport.gmk:
 
 - calculate the geomean and stdev of benchmark runs
 - write markdown snippets for rendering benchmark results as a chart on GitHub .MD files

Example:

helidon-quickstart-se$ tbjava ../lib/GithubMDChart.java < mainline_vs_premain.csv
run,mainline default,mainline custom static CDS,premain custom static CDS only,premain AOT new workflow
1,398,240,175,116
2,380,239,138,110
3,411,287,144,113
4,399,246,147,112
5,402,245,147,110
6,398,242,142,122
7,383,242,146,111
8,388,260,152,108
9,395,250,149,113
10,380,241,143,132
Geomean,393.28,248.84,148.01,114.51
Stdev,9.78,13.91,9.63,6.86

```mermaid
gantt
    title Elapsed time (ms)
    todayMarker off
    dateFormat  X
    axisFormat %s

    mainline default   : 0, 393.28
    mainline custom static CDS   : 0, 248.84
    premain custom static CDS only   : 0, 148.01
    premain AOT new workflow   : 0, 114.51
```

*/

import java.io.PrintWriter;
import java.util.Scanner;
import java.util.ArrayList;

public class GithubMDChart {
    @SuppressWarnings("unchecked")
    public static void main(String args[]) throws Exception {
        Scanner input = new Scanner(System.in);
        String line = input.nextLine();
        //System.out.println(line);
        String head[] = line.split(",");
        Object[] groups = new Object[head.length];
        String[] geomeans = new String[head.length];
        for (int i = 0; i < head.length; i++) {
            groups[i] = new ArrayList<Double>();
        }
        while (input.hasNext()) {
            line = input.nextLine();
            //System.out.println(line);
            String parts[] = line.split(",");
            for (int i = 0; i < head.length; i++) {
                ArrayList<Double> list =  (ArrayList<Double>)(groups[i]);
                list.add(Double.valueOf(Double.parseDouble(parts[i])));
            }
        }

        for (int i = 0; i < head.length; i++) {
            ArrayList<Double> list =  (ArrayList<Double>)(groups[i]);
            if (i == 0) {
                System.out.print("Geomean");
            } else {
                System.out.print(",");
                geomeans[i] = geomean(list);
                System.out.print(geomeans[i]);
            }
        }

        System.out.println();

        for (int i = 0; i < head.length; i++) {
            ArrayList<Double> list =  (ArrayList<Double>)(groups[i]);
            if (i == 0) {
                System.out.print("Stdev");
            } else {
                System.out.print(",");
                System.out.print(stdev(list));
            }
        }
        System.out.println();
        System.out.println("Markdown snippets in " + args[0]);

        PrintWriter pw = new PrintWriter(args[0]);
        pw.println("```mermaid");
        pw.println("gantt");
        pw.println("    title Elapsed time (ms, smaller is better)");
        pw.println("    todayMarker off");
        pw.println("    dateFormat  X");
        pw.println("    axisFormat %s");
        pw.println();

        for (int i = 1; i < head.length; i++) {
            pw.println("    " + head[i] + "   : 0, " + geomeans[i]);
        }
        pw.println("```");
        pw.println();
        pw.println("-----------------Normalized---------------------------------------------");

        pw.println("```mermaid");
        pw.println("gantt");
        pw.println("    title Elapsed time (normalized, smaller is better)");
        pw.println("    todayMarker off");
        pw.println("    dateFormat  X");
        pw.println("    axisFormat %s");
        pw.println();

        for (int i = 1; i < head.length; i++) {
            double base = Double.parseDouble(geomeans[1]);
            double me   = Double.parseDouble(geomeans[i]);
            pw.println("    " + head[i] + "   : 0, " + String.format("%.0f", 1000.0 * me / base));
        }
        pw.println("```");
        pw.close();
    }


    static String geomean(ArrayList<Double> list) {
        double log = 0.0d;
        for (Double d : list) {
            double v = d.doubleValue();
            if (v <= 0) {
                v = 0.000001;
            }
            log += Math.log(v);
        }

	return String.format("%.2f", Math.exp(log / list.size()));
    }

    static String stdev(ArrayList<Double> list) {
        double sum = 0.0;
        for (Double d : list) {
            sum += d.doubleValue();
        }

        double length = list.size();
        double mean = sum / length;

        double stdev = 0.0;
        for (Double d : list) {
            stdev += Math.pow(d.doubleValue() - mean, 2);
        }

	return String.format("%.2f", Math.sqrt(stdev / length));
    }
}
