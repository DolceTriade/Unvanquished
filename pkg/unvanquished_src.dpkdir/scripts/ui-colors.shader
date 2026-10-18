gfx/colors/white
{
	cull none
	{
		map $whiteimage
		blendfunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbgen vertex
	}
}

gfx/colors/plain
{
	cull disable
	{
		map $whiteimage
		rgbGen entity
		alphaGen entity
		blendFunc GL_SRC_ALPHA GL_ONE
	}
}

gfx/colors/backtile
{
	nopicmip
	nomipmaps
	{
		map $blackimage
	}
}

ui/assets/logos/overload_mod_loading
{
    cull disable
    sort nearest
    {
        videoMap ui/assets/logos/overload_mod_nebula_spiral.roq
        rgbGen vertex
        blendFunc add
    }
}
