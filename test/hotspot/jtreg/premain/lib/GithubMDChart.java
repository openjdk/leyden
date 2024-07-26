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
run,mainline default,mainline custom static CDS,premain custom static CDS only,premain CDS + AOT
1,398,244,144,107
2,387,247,142,108
3,428,238,143,107
4,391,252,142,111
5,417,247,141,107
6,390,239,139,127
7,387,247,145,111
8,387,240,147,110
9,388,242,147,108
10,400,242,167,108
Geomean,397.08,243.76,145.52,110.26
Stdev,13.55,4.19,7.50,5.73

```mermaid
---
config:
    xyChart:
        chartOrientation: horizontal
        height: 300
---
xychart-beta
    x-axis "variant" ["mainline default", "mainline custom static CDS", "premain custom static CDS only", "premain CDS + AOT"]
    y-axis "Elapsed time (ms, smaller is better)" 0 --> 397
    bar [397, 244, 146, 110]
```

*/

import java.io.PrintWriter;
import java.util.Scanner;
import java.util.ArrayList;
import java.util.Arrays;

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

        String[] norm = new String[geomeans.length];
        for (int i = 1; i < head.length; i++) {
            double base = Double.parseDouble(geomeans[1]);
            double me   = Double.parseDouble(geomeans[i]);
            norm[i] = String.format("%.0f", 1000.0 * me / base);
        }

        PrintWriter pw = new PrintWriter(args[0]);
        // Horizontal avoids issues with long names overlapping each other,
        // and setting a smaller-than-default height makes it easier to overlook and compare.
        pw.println("""
        ```mermaid
        ---
        config:
            xyChart:
                chartOrientation: horizontal
                height: 300
        ---
        xychart-beta
            x-axis "variant" [$names]
            y-axis "Elapsed time (ms, smaller is better)" 0 --> $maxtime
            bar [$geomeans]
        ```
        
        -----------------Normalized---------------------------------------------
        ```mermaid
        ---
        config:
            xyChart:
                chartOrientation: horizontal
                height: 300
        ---
        xychart-beta
            x-axis "variant" [$names]
            y-axis "Elapsed time (normalized, smaller is better)" 0 --> 1000
            bar [$norms]
        ```
        """
        .replace("$names", '"' + String.join("\", \"", Arrays.copyOfRange(head, 1, head.length)) + '"')
        .replace("$maxtime", geomeans[1])
        .replace("$geomeans", String.join(", ", Arrays.copyOfRange(geomeans, 1, geomeans.length)))
        .replace("$norms", String.join(", ", Arrays.copyOfRange(norm, 1, norm.length)))
        );
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
