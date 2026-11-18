import java.net.URL;
import java.net.URLClassLoader;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class URLClassLoaderFactory {
    // tests create new loaders and add them to this list to keep them alive
    static List<ClassLoader> loaderList = new ArrayList();

    public static ClassLoader createURLClassLoader(ClassLoader parent, String...args) throws Exception {
        List<URL> urls = new ArrayList();
        for (String jar: args) {
            File jarFile = new File(jar);
            urls.add(jarFile.toURI().toURL());
        }
        ClassLoader loader = new URLClassLoader(urls.toArray(new URL[0]), parent);
        loaderList.add(loader);
        return loader;
    }
}
