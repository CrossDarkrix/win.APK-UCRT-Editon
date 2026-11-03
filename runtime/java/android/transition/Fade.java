// android.transition.Fade -- fades a view's alpha in (on appear) or out (on
// disappear). The default member of AutoTransition for added/removed views, and a
// very common explicit transition (dialog/sheet content swaps, Fragment
// FADE transitions). Was absent. Mirrors
// frameworks/base/core/java/android/transition/Fade.java: onAppear animates alpha
// 0->1, onDisappear animates the captured alpha->0.
package android.transition;

import android.util.AttributeSet;
import android.content.Context;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.view.View;
import android.view.ViewGroup;

public class Fade extends Visibility {

    public static final int IN = MODE_IN;
    public static final int OUT = MODE_OUT;

    public Fade() {}

    public Fade(int fadingMode) {
        setMode(fadingMode);
    }

    /**
     * AOSP Fade.java:101-107 -- the XML-inflation constructor. Reads
     * android:fadingMode (attrs.xml: fade_in=1 / fade_out=2 / fade_in_out=3,
     * i.e. Visibility's MODE_IN / MODE_OUT bits) and defaults to the mode the
     * superclass already has.
     */
    public Fade(Context context, AttributeSet attrs) {
        super(context, attrs);
        int fadingMode = Transition.getEnumAttr(attrs, "fadingMode",
                new String[] { "fade_in", "fade_out", "fade_in_out" },
                new int[] { MODE_IN, MODE_OUT, MODE_IN | MODE_OUT },
                getMode());
        setMode(fadingMode);
    }

    @Override
    public Animator onAppear(ViewGroup sceneRoot, View view, TransitionValues startValues,
            TransitionValues endValues) {
        if (view == null) return null;
        final float endAlpha = view.getAlpha();
        view.setAlpha(0f);
        return createFadeAnimation(view, 0f, endAlpha <= 0f ? 1f : endAlpha);
    }

    @Override
    public Animator onDisappear(ViewGroup sceneRoot, View view, TransitionValues startValues,
            TransitionValues endValues) {
        if (view == null) return null;
        final float startAlpha = view.getAlpha();
        final Animator anim = createFadeAnimation(view, startAlpha, 0f);
        if (anim != null) {
            // AOSP restores the view's alpha after the disappear so a re-used view
            // is not left invisible.
            final View target = view;
            final float restore = startAlpha;
            anim.addListener(new AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(Animator animation) {
                    target.setAlpha(restore);
                }
            });
        }
        return anim;
    }

    private Animator createFadeAnimation(View view, float startAlpha, float endAlpha) {
        if (startAlpha == endAlpha) return null;
        view.setAlpha(startAlpha);
        return ObjectAnimator.ofFloat(view, "alpha", startAlpha, endAlpha);
    }
}
