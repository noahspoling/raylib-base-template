package __PACKAGE_NAME__;

import android.app.NativeActivity;
import android.os.Bundle;

public class NativeLoader extends NativeActivity {

    static {
        System.loadLibrary("__PROJECT_NAME__");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }
}
