/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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

package jdk.test.lib.cds;

import java.io.File;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.StringArrayUtils;

/*
 * This is a base class used for testing CDS functionalities with complex applications.
 * You can define the application by overridding the vmArgs(), classpath() and appCommandLine()
 * methods. Application-specific validation checks can be implemented with checkExecution().
 *
 * Note: to debug the new workflow, run jtreg with -vmoption:-DCDSAppTester.split.new.workflow=true
 * This will run the new workflow in two separate processes that you can rerun easily inside a debugger.
 * Also, the log files are easier to read.
*/
abstract public class CDSAppTester {
    private enum Workflow {
        STATIC,        // classic -Xshare:dump workflow, without any Leyden optimizations
        DYNAMIC,       // classic -XX:ArchiveClassesAtExit, without any Leyden optimizations
        LEYDEN_OLD,    // The old "5 step workflow", to be phased out
        LEYDEN,        // The new "one step training workflow" -- see JDK-8320264
    }

    public enum RunMode {
        CLASSLIST,
        DUMP_STATIC,
        DUMP_DYNAMIC,
        DUMP_CODECACHE,    // LEYDEN_OLD only
        TRAINING,          // LEYDEN only
        TRAINING0,         // LEYDEN only
        TRAINING1,         // LEYDEN only
        PRODUCTION;

        public boolean isStaticDump() {
            return this == TRAINING1 || this == DUMP_STATIC;
        }
        public boolean isProductionRun() {
            return this == PRODUCTION;
        }
    }

    public final String name() {
        return this.name;
    }

    // optional
    public String[] vmArgs(RunMode runMode) {
        return new String[0];
    }

    // optional
    public String classpath(RunMode runMode) {
        return null;
    }

    // must override
    // main class, followed by arguments to the main class
    abstract public String[] appCommandLine(RunMode runMode);

    // optional
    public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {}

    private Workflow workflow;

    public final boolean isStaticWorkflow() {
        return workflow == Workflow.STATIC;
    }

    public final boolean isDynamicWorkflow() {
        return workflow == Workflow.DYNAMIC;
    }

    public final boolean isLeydenOldWorkflow() {
        return workflow == Workflow.LEYDEN_OLD;
    }

    public final boolean isLeydenWorkflow() {
        return workflow == Workflow.LEYDEN;
    }

    private String classListLog() {
        return "-Xlog:class+load=debug:file=" + classListFile + ".log";
    }
    private String staticDumpLog() {
        return "-Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=" + staticArchiveFile + ".log::filesize=0";
    }
    private String dynamicDumpLog() {
        return "-Xlog:cds=debug,cds+class=debug,cds+resolve=debug,class+load=debug:file=" + dynamicArchiveFile + ".log::filesize=0";
    }
    private String codeCacheDumpLog() {
        return "-Xlog:scc:file=" + codeCacheFile + ".log::filesize=0";
    }
    private String oldProductionRunLog() {
        return "-Xlog:scc*=warning,cds:file=" + name() + ".old-production.log::filesize=0";
    }

    private final String name;
    private final String classListFile;
    private final String staticArchiveFile;
    private final String dynamicArchiveFile;
    private final String codeCacheFile;

    private String trainingLog() {
        return "-Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=" + cdsFile + ".log:uptime,level,tags,pid:filesize=0";
    }

    private String trainingLog0() {
        return "-Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=" + cdsFile + ".training0.log::filesize=0";
    }

    private String trainingLog1() {
        return "-Xlog:cds=debug,cds+class=debug,cds+heap=warning,cds+resolve=debug:file=" + cdsFile + ".training1.log::filesize=0";
    }

    private String productionRunLog() {
        return "-Xlog:scc*=warning,cds:file=" + name() + ".production.log::filesize=0";
    }

    private final String cdsFile; // -XX:CacheDataStore=<foo>.cds
    private final String aotFile; // = cdsFile + ".code"

    public CDSAppTester(String name) {
        // Old workflow
        this.name = name;
        classListFile = name() + ".classlist";
        staticArchiveFile = name() + ".static.jsa";
        dynamicArchiveFile = name() + ".dynamic.jsa";
        codeCacheFile = name() + ".code.jsa";

        // New workflow
        cdsFile = name() + ".cds";
        aotFile = cdsFile + ".code";
    }

    private void listOutputFile(String file) {
        File f = new File(file);
        if (f.exists()) {
            System.out.println("[output file: " + file + " " + f.length() + " bytes]");
        } else {
            System.out.println("[output file: " + file + " does not exist]");
        }
    }

    private void checkExecutionHelper(OutputAnalyzer output, RunMode runMode) throws Exception {
        output.shouldHaveExitValue(0);
        CDSTestUtils.checkCommonExecExceptions(output);
        checkExecution(output, runMode);
    }

    private OutputAnalyzer createClassList() throws Exception {
        RunMode runMode = RunMode.CLASSLIST;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), classListLog(),
                                                   "-Xshare:off",
                                                   "-XX:DumpLoadedClassList=" + classListFile,
                                                   "-cp", classpath(runMode));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "classlist");
        listOutputFile(classListFile);
        listOutputFile(classListFile + ".log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    private OutputAnalyzer dumpStaticArchive() throws Exception {
        RunMode runMode = RunMode.DUMP_STATIC;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), staticDumpLog(),
                                                   "-Xlog:cds",
                                                   "-Xlog:cds+heap=error",
                                                   "-Xshare:dump",
                                                   "-XX:SharedArchiveFile=" + staticArchiveFile,
                                                   "-XX:SharedClassListFile=" + classListFile,
                                                   "-cp", classpath(runMode));
        if (isLeydenOldWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine,
                                              "-XX:+ArchiveInvokeDynamic",
                                              "-XX:+ArchiveDynamicProxies",
                                              "-XX:+ArchiveReflectionData");
        }

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "static");
        listOutputFile(staticArchiveFile);
        listOutputFile(staticArchiveFile + ".log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    private OutputAnalyzer dumpDynamicArchive() throws Exception {
        RunMode runMode = RunMode.DUMP_DYNAMIC;
        String[] cmdLine;
        if (isDynamicWorkflow()) {
          // "classic" dynamic archive
          cmdLine = StringArrayUtils.concat(vmArgs(runMode), dynamicDumpLog(),
                                            "-Xlog:cds",
                                            "-XX:ArchiveClassesAtExit=" + dynamicArchiveFile,
                                            "-cp", classpath(runMode));
        } else {
          // Leyden "OLD" workflow step 3
          cmdLine = StringArrayUtils.concat(vmArgs(runMode), dynamicDumpLog(),
                                            "-Xlog:cds",
                                            "-XX:ArchiveClassesAtExit=" + dynamicArchiveFile,
                                            "-XX:SharedArchiveFile=" + staticArchiveFile,
                                            "-XX:+RecordTraining",
                                            "-cp", classpath(runMode));
        }
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "dynamic");
        listOutputFile(dynamicArchiveFile);
        listOutputFile(dynamicArchiveFile + ".log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    private OutputAnalyzer dumpCodeCache() throws Exception {
        RunMode runMode = RunMode.DUMP_CODECACHE;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), codeCacheDumpLog(),
                                                   "-XX:SharedArchiveFile=" + dynamicArchiveFile,
                                                   "-XX:+ReplayTraining",
                                                   "-XX:+StoreCachedCode",
                                                   "-XX:CachedCodeFile=" + codeCacheFile,
                                                   "-XX:CachedCodeMaxSize=512M",
                                                   "-cp", classpath(runMode));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "code");
        listOutputFile(codeCacheFile);
        listOutputFile(codeCacheFile + ".log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    private OutputAnalyzer oldProductionRun() throws Exception {
        RunMode runMode = RunMode.PRODUCTION;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), oldProductionRunLog(),
                                                   "-XX:SharedArchiveFile=" + dynamicArchiveFile,
                                                   "-XX:+ReplayTraining",
                                                   "-XX:+LoadCachedCode",
                                                   "-XX:CachedCodeFile=" + codeCacheFile,
                                                   "-cp", classpath(runMode));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "old-production");
        listOutputFile(name() + ".old-production.log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    // normal training workflow (main JVM process spawns child process)
    private OutputAnalyzer trainingRun() throws Exception {
        RunMode runMode = RunMode.TRAINING;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), trainingLog(),
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-cp", classpath(runMode),
                                                   // Use PID to distinguish the logs of the training process
                                                   // and the forked final image dump process.
                                                   "-Xlog:cds::uptime,level,tags,pid");
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "training");
        listOutputFile(cdsFile);
        listOutputFile(cdsFile + ".log");   // The final dump
        listOutputFile(cdsFile + ".log.0"); // the preimage dump
        checkExecutionHelper(output, runMode);
        return output;
    }

    // "split" training workflow (launch the two processes manually, for easier debugging);
    private OutputAnalyzer trainingRun0() throws Exception {
        RunMode runMode = RunMode.TRAINING0;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), trainingLog0(),
                                                   "-XX:+UnlockDiagnosticVMOptions",
                                                   "-XX:+CDSManualFinalImage",
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-cp", classpath(runMode));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "training0");
        listOutputFile(cdsFile);
        listOutputFile(cdsFile + ".0.log");
        checkExecutionHelper(output, runMode);
        return output;
    }
    private OutputAnalyzer trainingRun1() throws Exception {
        RunMode runMode = RunMode.TRAINING1;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), trainingLog1(),
                                                   "-XX:+UnlockDiagnosticVMOptions",
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-XX:CDSPreimage=" + cdsFile + ".preimage",
                                                   "-cp", classpath(runMode));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "training1");
        listOutputFile(cdsFile);
        listOutputFile(cdsFile + ".1.log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    private OutputAnalyzer productionRun() throws Exception {
        RunMode runMode = RunMode.PRODUCTION;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode), productionRunLog(),
                                                   "-cp", classpath(runMode));
        if (isStaticWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:SharedArchiveFile=" + staticArchiveFile);
        } else if (isDynamicWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:SharedArchiveFile=" + dynamicArchiveFile);
        } else {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:CacheDataStore=" + cdsFile);
        }

        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, "production");
        listOutputFile(name() + ".production.log");
        checkExecutionHelper(output, runMode);
        return output;
    }

    public void run(String args[]) throws Exception {
        if (args.length == 1) {
            if (args[0].equals("STATIC")) {
                runStaticWorkflow();
                return;
            }
            if (args[0].equals("DYNAMIC")) {
                runDynamicWorkflow();
                return;
            }
            if (args[0].equals("LEYDEN_OLD")) {
                runLeydenOldWorkflow();
                return;
            }
            if (args[0].equals("LEYDEN")) {
                runLeydenWorkflow();
                return;
            }
        }

        throw new RuntimeException("Must have exactly one command line argument: STATIC, DYNAMIC, LEYDEN_OLD or LEYDEN");
    }

    private void runStaticWorkflow() throws Exception {
        this.workflow = Workflow.STATIC;
        createClassList();
        dumpStaticArchive();
        productionRun();
    }

    private void runDynamicWorkflow() throws Exception {
        this.workflow = Workflow.DYNAMIC;
        dumpDynamicArchive();
        productionRun();
    }

    private void runLeydenOldWorkflow() throws Exception {
        this.workflow = Workflow.LEYDEN_OLD;
        createClassList();
        dumpStaticArchive();
        dumpDynamicArchive();
        dumpCodeCache();
        oldProductionRun();
    }

    private void runLeydenWorkflow() throws Exception {
        this.workflow = Workflow.LEYDEN;
        if (System.getProperty("CDSAppTester.split.new.workflow") != null) {
            trainingRun0();
            trainingRun1();
        } else {
            trainingRun();
        }
        productionRun();
    }
}
