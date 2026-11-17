package com.example.lemonchat;

import android.content.Context;

import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

import static org.junit.Assert.*;

/** checks that the hybrid app keeps its existing data namespace */
@RunWith(AndroidJUnit4.class)
public class ExampleInstrumentedTest
{
    @Test
    public void useAppContext()
    {
        Context appContext = InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertEquals(appContext.getPackageName() + ".test", InstrumentationRegistry.getInstrumentation().getContext().getPackageName());
    }
}
