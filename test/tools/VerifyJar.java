import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * 逐类加载校验：把 jar 内每个 .class 都送进校验器（含永远不会执行到的分支）。
 *
 * 为什么不用 `java -cp out.jar Main`：正常启动只校验实际执行到的路径，而改写
 * 方法体的错误往往落在没被执行到的分支上。
 *
 * initialize=false 是刻意的：链接（含校验）先于初始化完成，所以校验照跑，但不会
 * 触发静态初始化——语料里的样本可能开窗口、调 System.exit，初始化会挂住。
 *
 * 用法：java -Xverify:all VerifyJar.java <jar> [--quiet]
 * 输出：classes=N ok=N failed=N；失败明细打 stderr；退出码 1 表示有失败。
 */
public final class VerifyJar {

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java VerifyJar.java <jar> [--quiet]");
            System.exit(2);
        }
        final File jar = new File(args[0]);
        final boolean quiet = args.length > 1 && args[1].equals("--quiet");

        List<String> names = new ArrayList<>();
        try (ZipFile zf = new ZipFile(jar)) {
            Enumeration<? extends ZipEntry> en = zf.entries();
            while (en.hasMoreElements()) {
                String n = en.nextElement().getName();
                if (!n.endsWith(".class")) continue;
                if (n.startsWith("META-INF/")) continue;   // multi-release / 签名残留
                names.add(n.substring(0, n.length() - 6).replace('/', '.'));
            }
        }

        // 父 loader 置 null：只用这个 jar 自己的类，避免宿主机 classpath 兜底把
        // 本该失败的加载变成成功。
        URLClassLoader cl = new URLClassLoader(new URL[] {jar.toURI().toURL()}, null);

        int ok = 0;
        List<String> failed = new ArrayList<>();
        for (String n : names) {
            try {
                Class.forName(n, false, cl);
                ok++;
            } catch (Throwable t) {
                failed.add(n);
                if (!quiet) {
                    System.err.println("FAIL " + n);
                    System.err.println("     " + t.getClass().getName() + ": " + t.getMessage());
                    StackTraceElement[] st = t.getStackTrace();
                    for (int i = 0; i < Math.min(3, st.length); i++) {
                        System.err.println("     at " + st[i]);
                    }
                }
            }
        }
        System.out.printf("classes=%d ok=%d failed=%d%n", names.size(), ok, failed.size());
        System.exit(failed.isEmpty() ? 0 : 1);
    }
}
