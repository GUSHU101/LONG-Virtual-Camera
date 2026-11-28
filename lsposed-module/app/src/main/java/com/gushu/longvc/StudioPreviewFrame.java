package com.gushu.longvc;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.FrameLayout;

public final class StudioPreviewFrame extends FrameLayout {
    public StudioPreviewFrame(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int maximum = Math.round(220 * getResources().getDisplayMetrics().density);
        int height = Math.min(maximum, width * 9 / 16);
        super.onMeasure(widthMeasureSpec, MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
    }
}
