/*
 * bbbchatofx.cpp
 *
 * vim: ts=8 sw=8
 *
 * Main entry point for plugin
 *
 * Copyright (c) 2022-2023 Sylvain Munaut <tnt@246tNt.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pugixml.hpp>

#include <cairomm/context.h>
#include <pangomm/init.h>
#include <pangomm/layout.h>
#include <pangomm/fontdescription.h>
#include <pangomm/fontfamily.h>
#include <pangomm/fontmap.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMultiThread.h"
#include "ofxPixels.h"

#if defined __APPLE__ || defined linux || defined __FreeBSD__
#  define EXPORT OfxExport __attribute__((visibility("default")))
#else
#  define EXPORT OfxExport
#endif


/* -------------------------------------------------------------------------- */
/* Chat Message                                                               */
/* -------------------------------------------------------------------------- */

class ChatMessage
{
private:
	double      m_time;
	std::string m_user;
	std::string m_text;

protected:
	ChatMessage(double time, std::string const &user, std::string const &text);

public:
	virtual ~ChatMessage();

	bool operator < (ChatMessage const &other);
	friend bool operator < (ChatMessage const &text, double time);
	friend bool operator < (double time, ChatMessage const &text);

	double             time() const { return m_time; }
	std::string const &user() const { return m_user; }
	std::string const &text() const { return m_text; }

	static std::vector<ChatMessage> loadFromFile(const char *filename);

};

ChatMessage::ChatMessage(double time, std::string const &user, std::string const &text)
	: m_time{time}, m_user{user}, m_text{text}
{
	/* Nothing */
}

ChatMessage::~ChatMessage()
{
	/* Nothing */
}


bool
ChatMessage::operator < (ChatMessage const &other)
{
	return this->m_time < other.m_time;
}

bool
operator < (ChatMessage const &msg,double time)
{
	return msg.m_time < time;
}

bool
operator < (double time, ChatMessage const &msg)
{
	return time < msg.m_time;
}


std::vector<ChatMessage>
ChatMessage::loadFromFile(const char *filename)
{
	std::vector<ChatMessage> rv;
	pugi::xml_document doc;

	doc.load_file(filename);

	for (pugi::xml_node element : doc.root().child("popcorn").children())
	{
		double      time = element.attribute("in").as_double();
		const char *name = element.attribute("name").value();
		const char *msg  = element.attribute("message").value();

		rv.push_back(ChatMessage(time, name, msg));
	}

	sort(rv.begin(), rv.end()); /* should already be sorted, but make sure */

	return rv;
}



/* -------------------------------------------------------------------------- */
/* Chat Message Renderer                                                        */
/* -------------------------------------------------------------------------- */

class ChatMessageRenderer
{
protected:
	std::vector<ChatMessage> m_messages;

	Cairo::RefPtr<Cairo::ImageSurface> m_surface;
	Cairo::RefPtr<Cairo::Context>      m_context;
	Glib::RefPtr<Pango::Layout>        m_layout;

	int m_width;
	int m_height;
	int m_margin;

	double m_color_bg[4];
	double m_color_user[3];
	double m_color_text[3];

	double m_fade_in_time;
	double m_hold_time;
	double m_fade_out_time;

	void drawInit();
	void drawRelease();
	void drawBackground(int text_width, int text_height, double alpha);
	void drawMessage(ChatMessage const &msg, double time, int &height, double &progress);

public:
	ChatMessageRenderer(std::vector<ChatMessage> messages);
	virtual ~ChatMessageRenderer();

	void setMessages(const std::vector<ChatMessage> &messages) { this->m_messages = messages; }

	void setWidth(int width)   { if (this->m_width  != width)  { this->m_width  = width;  this->drawRelease(); } }
	void setHeight(int height) { if (this->m_height != height) { this->m_height = height; this->drawRelease(); } }
	void setMargin(int margin) { if (this->m_margin != margin) { this->m_margin = margin; this->drawRelease(); } }

	int getWidth()  const { return this->m_width;  }
	int getHeight() const { return this->m_height; }

	void setColorBackground(const double c[4]) { for (int i=0; i<4; i++) this->m_color_bg[i]   = c[i]; };
	void setColorUser      (const double c[3]) { for (int i=0; i<3; i++) this->m_color_user[i] = c[i]; };
	void setColorText      (const double c[3]) { for (int i=0; i<3; i++) this->m_color_text[i] = c[i]; };

	void setFadeInTime (double t) { this->m_fade_in_time  = t; }
	void setHoldTime   (double t) { this->m_hold_time     = t; }
	void setFadeOutTime(double t) { this->m_fade_out_time = t; }

	int  render(double time);

	int            getStride() const { return this->m_surface->get_stride(); }
	const uint8_t *getData()   const { return (const uint8_t*) this->m_surface->get_data();   }
};

ChatMessageRenderer::ChatMessageRenderer(std::vector<ChatMessage> messages = {})
	: m_messages(messages),
	m_width{640}, m_height{360}, m_margin{10},
	m_color_bg{0.5, 0.5, 0.5, 0.5}, m_color_user{0.628, 0.0, 0.0}, m_color_text{0.0, 0.0, 0.0},
	m_fade_in_time{1}, m_hold_time{15.0}, m_fade_out_time{1}
{
	/* Do the global Pango init here */
	Pango::init();
}

ChatMessageRenderer::~ChatMessageRenderer()
{
	/* Nothing */
}

void
ChatMessageRenderer::drawInit()
{
	/* Do we have drawing objects ready ?
	 *   If no  -> Create and init new ones
	 *   If yes -> Clear the surface
	 */
	if (!this->m_surface) {
		/* Create a render surface */
		this->m_surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, this->m_width, this->m_height);

		/* Create a context for it */
		this->m_context = Cairo::Context::create(this->m_surface);

		/* Disable sub-pixel rendering on context ! */
		Cairo::FontOptions font_options;
		font_options.set_antialias((Cairo::Antialias)5); /* Cairo::Antialias::ANTIALIAS_GRA doesn't work !!! */
		this->m_context->set_font_options(font_options);

		/* Create layout */
		this->m_layout = Pango::Layout::create(this->m_context);

		this->m_layout->set_width  ((this->m_width  - 2 * this->m_margin) * PANGO_SCALE);
		this->m_layout->set_height ((this->m_height - 2 * this->m_margin) * PANGO_SCALE);

		Pango::FontDescription font_description("Sans 12");
		this->m_layout->set_font_description(font_description);

		/* Set the alignment and wrapping */
		this->m_layout->set_alignment(Pango::ALIGN_LEFT);
		this->m_layout->set_wrap(Pango::WRAP_WORD_CHAR);
	} else {
		/* Clear the surface */
		this->m_context->save();
		this->m_context->set_operator(Cairo::OPERATOR_CLEAR);
		this->m_context->paint();
		this->m_context->restore();
	}
}

void
ChatMessageRenderer::drawRelease()
{
	/* Release graphic objects */
	this->m_layout.clear();
	this->m_context.clear();
	this->m_surface.clear();
};

void
ChatMessageRenderer::drawBackground(int text_width, int text_height, double alpha)
{
	double deg = M_PI / 180.0;
	double r = 1.5 * this->m_margin;
	double x = 0.0;
	double y = 0.0;
	double w = text_width  + 2 * this->m_margin;
	double h = text_height + 2 * this->m_margin;

	this->m_context->set_source_rgba(
		this->m_color_bg[0],
		this->m_color_bg[1],
		this->m_color_bg[2],
		this->m_color_bg[3] * alpha
	);

	this->m_context->begin_new_sub_path();
	this->m_context->arc (x + w - r, y + r,     r, -90 * deg,   0 * deg);
	this->m_context->arc (x + w - r, y + h - r, r,   0 * deg,  90 * deg);
	this->m_context->arc (x + r,     y + h - r, r,  90 * deg, 180 * deg);
	this->m_context->arc (x + r,     y + r,     r, 180 * deg, 270 * deg);
	this->m_context->close_path();
	this->m_context->fill();
}

void
ChatMessageRenderer::drawMessage(ChatMessage const &msg, double time, int &height, double &progress)
{
	/* Compute the alpha for this message */
	double alpha = 1.0;
	double t_hold = msg.time() + this->m_fade_in_time;
	double t_fout = msg.time() + this->m_fade_in_time + this->m_hold_time;

	progress = 1.0;

	if (time < t_hold) {
		alpha = (time - msg.time()) / this->m_fade_in_time;
		progress = alpha;
	} else if (time > t_fout)
		alpha = 1.0 - (time - t_fout) / this->m_fade_out_time;

	/* If we're "too transparent", just skip */
	if (alpha <= 1 / 255.0) {
		height = 0;
		return;
	}

	/* Set markup content */
	char *markup = g_markup_printf_escaped(
		"<span weight='bold' color='#%02x%02x%02x%02x'>[%s]</span> %s",
		(int)(this->m_color_user[0] * 255),
		(int)(this->m_color_user[1] * 255),
		(int)(this->m_color_user[2] * 255),
		(int)(alpha * 255),
		msg.user().c_str(),
		msg.text().c_str()
	);
	this->m_layout->set_markup(markup);
	g_free(markup);

	/* Get final layout dimensions */
	int text_width, text_height;
	this->m_layout->get_size(text_width, text_height);
	text_width  = PANGO_PIXELS(text_width);
	text_height = PANGO_PIXELS(text_height);

	/* Draw background */
	this->drawBackground(text_width, text_height, alpha);

	/* Render the layout */
	this->m_context->set_source_rgba(
		this->m_color_text[0],
		this->m_color_text[1],
		this->m_color_text[2],
		alpha
	);
	this->m_context->move_to(this->m_margin, this->m_margin);
	this->m_layout->show_in_cairo_context(this->m_context);

	/* Returned the used Y space */
	height = text_height + 3 * this->m_margin;
}

int
ChatMessageRenderer::render(double time)
{
	/* Find relevant messages */
	double time_lo = time - (this->m_fade_in_time + this->m_hold_time + this->m_fade_out_time);
	double time_hi = time;

	const auto first = std::lower_bound(this->m_messages.begin(), this->m_messages.end(), time_lo);
	const auto last  = std::upper_bound(first,                    this->m_messages.end(), time_hi);
	std::span<ChatMessage> active_messages = { first, last };

	/* If nothing to render, skip */
	if (active_messages.empty())
		return 0;

	/* Setup surface and context */
	this->drawInit();

	/* Setup margin */
	this->m_context->set_identity_matrix();
	this->m_context->translate(this->m_margin, 0);

	/* Iterate over active messages and acculumate total height */
	double th = 0.0;

	for (const auto& msg : active_messages)
	{
		double p;
		int h;
		this->drawMessage(msg, time, h, p);
		this->m_context->translate(0, h);
		th += p * h;
	}

	/* Finish */
	this->m_surface->flush();

	return (int)round(th);
}



/* ------------------------------------------------------------------------- */
/* OFX Globals                                                               */
/* ------------------------------------------------------------------------- */

/* Pointers to various bits of the host */
static OfxHost *		gHost;
static OfxImageEffectSuiteV1 *	gEffectHost;
static OfxPropertySuiteV1 *	gPropHost;
static OfxParameterSuiteV1 *	gParamHost;
static OfxMultiThreadSuiteV1 *  gThreadHost;



/* ------------------------------------------------------------------------- */
/* Private Data                                                              */
/* ------------------------------------------------------------------------- */

struct InstanceData {
	/* Clips Handles */
	OfxImageClipHandle outputClip;

	/* Params Handles */
	OfxParamHandle dataFileParam;
	OfxParamHandle renderSizeParam;
	OfxParamHandle fontFamilyParam;
	OfxParamHandle fontSizeParam;
	OfxParamHandle bgColorParam;
	OfxParamHandle userColorParam;
	OfxParamHandle textColorParam;
	OfxParamHandle fadeInTimeParam;
	OfxParamHandle holdTimeParam;
	OfxParamHandle fadeOutTimeParam;

	/* Params */
	bool needReconfig;
	double framerate;

	/* Chat */
	OfxMutexHandle mutex;
	ChatMessageRenderer cmr;
};

static InstanceData *
getInstanceData(OfxImageEffectHandle effect)
{
	InstanceData *priv = NULL;
	OfxPropertySetHandle effectProps;

	gEffectHost->getPropertySet(effect, &effectProps);
	gPropHost->propGetPointer(effectProps,
		kOfxPropInstanceData, 0,
		(void **) &priv);

	return priv;
}



/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void
doReconfigure(OfxImageEffectHandle effect)
{
	InstanceData *priv = getInstanceData(effect);
	double data[4];

	gThreadHost->mutexLock(priv->mutex);

	gParamHost->paramGetValue(priv->renderSizeParam,  &data[0], &data[1]);
	priv->cmr.setWidth ((int)round(data[0]));
	priv->cmr.setHeight((int)round(data[1]));

	//gParamHost->paramGetValue(priv->fontFamilyParam,  );
	// FIXME

	gParamHost->paramGetValue(priv->fontSizeParam,    &data[0]);
	// FIXME

	gParamHost->paramGetValue(priv->bgColorParam,     &data[0], &data[1], &data[2], &data[3]);
	priv->cmr.setColorBackground(data);

	gParamHost->paramGetValue(priv->userColorParam,   &data[0], &data[1], &data[2]);
	priv->cmr.setColorUser(data);

	gParamHost->paramGetValue(priv->textColorParam,   &data[0], &data[1], &data[2]);
	priv->cmr.setColorText(data);

	gParamHost->paramGetValue(priv->fadeInTimeParam,  &data[0]);
	priv->cmr.setFadeInTime(data[0]);

	gParamHost->paramGetValue(priv->holdTimeParam,    &data[0]);
	priv->cmr.setHoldTime(data[0]);

	gParamHost->paramGetValue(priv->fadeOutTimeParam, &data[0]);
	priv->cmr.setFadeOutTime(data[0]);

	gThreadHost->mutexUnLock(priv->mutex);
}

static void
doReloadMessages(OfxImageEffectHandle effect)
{
	InstanceData *priv = getInstanceData(effect);

	/* Get the selected filename */
	char *data_file;
	gParamHost->paramGetValue(priv->dataFileParam, &data_file);

	/* Try to load and fail if invalid */
	gThreadHost->mutexLock(priv->mutex);

	try {
		priv->cmr.setMessages(ChatMessage::loadFromFile(data_file));
	} catch(std::exception &) {
		priv->cmr.setMessages({});
	}

	gThreadHost->mutexUnLock(priv->mutex);
}



/* ------------------------------------------------------------------------- */
/* kOfxMultiThreadSuite replacement                                          */
/* ------------------------------------------------------------------------- */

/*
 * From natron's code (GPLv2+)
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 * https://github.com/NatronGitHub/openfx-supportext/blob/master/ofxsThreadSuite.cpp
 */

static OfxStatus
mutexCreate(OfxMutexHandle *mutex, int lockCount)
{
    if (!mutex)
        return kOfxStatFailed;

    try {
        std::recursive_mutex* m = new std::recursive_mutex();
        for (int i = 0; i < lockCount; ++i) {
            m->lock();
        }
        *mutex = (OfxMutexHandle)(m);
        return kOfxStatOK;
    } catch (std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatErrUnknown;
    }
}

static OfxStatus
mutexLock(const OfxMutexHandle mutex)
{
	if (mutex == 0)
		return kOfxStatErrBadHandle;

	try {
		reinterpret_cast<std::recursive_mutex*>(mutex)->lock();
		return kOfxStatOK;
	} catch (std::bad_alloc&) {
		return kOfxStatErrMemory;
	} catch (...) {
		return kOfxStatErrUnknown;
	}
}

static OfxStatus
mutexUnLock(const OfxMutexHandle mutex)
{
    if (mutex == 0)
        return kOfxStatErrBadHandle;

    try {
        reinterpret_cast<std::recursive_mutex*>(mutex)->unlock();
        return kOfxStatOK;
    } catch (std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatErrUnknown;
    }
}

static OfxStatus
mutexTryLock(const OfxMutexHandle mutex)
{
    if (mutex == 0)
        return kOfxStatErrBadHandle;

    try {
        if ( reinterpret_cast<std::recursive_mutex*>(mutex)->try_lock() ) {
            return kOfxStatOK;
        } else {
            return kOfxStatFailed;
        }
    } catch (std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatErrUnknown;
    }
}

static OfxMultiThreadSuiteV1 cThreadLocal = {
	.mutexCreate  = mutexCreate,
	.mutexLock    = mutexLock,
	.mutexUnLock  = mutexUnLock,
	.mutexTryLock = mutexTryLock,
};



/* ------------------------------------------------------------------------- */
/* API Handlers                                                              */
/* ------------------------------------------------------------------------- */

static OfxStatus
effectLoad(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	/* Fetch the host suites out of the global host pointer */
	if(!gHost)
		return kOfxStatErrMissingHostFeature;

	gEffectHost     = (OfxImageEffectSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
	gPropHost       = (OfxPropertySuiteV1 *)    gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
	gParamHost	= (OfxParameterSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
	gThreadHost     = (OfxMultiThreadSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);
	if(!gEffectHost || !gPropHost || !gParamHost)
		return kOfxStatErrMissingHostFeature;

	if (!gThreadHost) {
		gThreadHost = &cThreadLocal;
	} else if (true) {
		// Always use our own ...
		gThreadHost = &cThreadLocal;
	}

	return kOfxStatOK;
}


static OfxStatus
effectUnload(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	/* Reset */
	gEffectHost = NULL;
	gPropHost   = NULL;
	gParamHost  = NULL;

	return kOfxStatOK;
}


static OfxStatus
effectCreateInstance(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	/* Get a pointer to the effect properties */
	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	/* Get a pointer to the effect's parameter set */
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

	/* Create private instance data holder */
	InstanceData *priv = new InstanceData();

	/* Mutex to protect renderer access */
	gThreadHost->mutexCreate(&priv->mutex, -1);

	/* Cache away clip handles */
	gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &priv->outputClip, 0);

	/* Cache away param handles */
	gParamHost->paramGetHandle(paramSet, "dataFile",    &priv->dataFileParam, 0);
	gParamHost->paramGetHandle(paramSet, "renderSize",  &priv->renderSizeParam, 0);
	gParamHost->paramGetHandle(paramSet, "fontFamily",  &priv->fontFamilyParam, 0);
	gParamHost->paramGetHandle(paramSet, "fontSize",    &priv->fontSizeParam, 0);
	gParamHost->paramGetHandle(paramSet, "bgColor",     &priv->bgColorParam, 0);
	gParamHost->paramGetHandle(paramSet, "userColor",   &priv->userColorParam, 0);
	gParamHost->paramGetHandle(paramSet, "textColor",   &priv->textColorParam, 0);
	gParamHost->paramGetHandle(paramSet, "fadeInTime",  &priv->fadeInTimeParam, 0);
	gParamHost->paramGetHandle(paramSet, "holdTime",    &priv->holdTimeParam, 0);
	gParamHost->paramGetHandle(paramSet, "fadeOutTime", &priv->fadeOutTimeParam, 0);

	/* Cache some other things */
	gPropHost->propGetDouble(effectProps, kOfxImageEffectPropFrameRate, 0, &priv->framerate);

	/* Set private instance data */
	gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) priv);

	/* Initial config */
	doReconfigure(effect);
	doReloadMessages(effect);

	return kOfxStatOK;
}


static OfxStatus
effectDestroyInstance(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);

	if (!priv)
		return kOfxStatOK;

	if (priv->mutex)
		gThreadHost->mutexDestroy(priv->mutex);

	delete priv;

	return kOfxStatOK;
}


static OfxStatus
effectInstanceChanged(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);

	/* See why it changed */
	char *changeReason;
	gPropHost->propGetString(inArgs, kOfxPropChangeReason, 0, &changeReason);

	/* We are only interested in user edits */
	if (strcmp(changeReason, kOfxChangeUserEdited) != 0)
		return kOfxStatReplyDefault;

	/* Fetch the type & name of the object that changed */
	char *typeChanged;
	gPropHost->propGetString(inArgs, kOfxPropType, 0, &typeChanged);

	//bool isClip  = strcmp(typeChanged, kOfxTypeClip) == 0;
	bool isParam = strcmp(typeChanged, kOfxTypeParameter) == 0;

	char *objChanged;
	gPropHost->propGetString(inArgs, kOfxPropName, 0, &objChanged);

	/* If the datafile changed, try to reload it */
	if (isParam && !strcmp(objChanged, "dataFile")) {
		doReloadMessages(effect);
		return kOfxStatOK;
	}

	/* If one of the dynamic param changed, mark as dirty and we'll refresh all at once */
	if (isParam && (
	    !strcmp(objChanged, "renderSize") ||
	    !strcmp(objChanged, "fontFamily") ||
	    !strcmp(objChanged, "fontSize")   ||
	    !strcmp(objChanged, "bgColor")    ||
	    !strcmp(objChanged, "userColor")  ||
	    !strcmp(objChanged, "textColor")  ||
	    !strcmp(objChanged, "fadeInTime") ||
	    !strcmp(objChanged, "holdTime")   ||
	    !strcmp(objChanged, "fadeOutTime")))
	{
		priv->needReconfig = true;
		return kOfxStatOK;
	}

	/* Don't trap any others */
	return kOfxStatReplyDefault;
}


static OfxStatus
effectEndInstanceChanged(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);

	/* Reconfigure if anything is pending */
	if (priv->needReconfig) {
		doReconfigure(effect);
		priv->needReconfig = false;
		return kOfxStatOK;
	}

	/* Nothing to do really */
	return kOfxStatReplyDefault;
}


static OfxStatus
effectDescribe(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	/* Get the property handle for the plugin */
	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	/* Identity / Classification */
	gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "OFX BBB Chat Renderer");
	gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "OpenFX");

	/* Applicable contexts */
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGenerator);

	/* We only deal with bytes */
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);

	/* Don't allow tiling, we need the full images at once */
	gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, 0);

	return kOfxStatOK;
}


static OfxStatus
effectDescribeInContext(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	OfxPropertySetHandle props;

	/* Check it's kOfxImageEffectContextGenerator */
	char *context;
	gPropHost->propGetString(inArgs, kOfxImageEffectPropContext, 0, &context);
	if (strcmp(context, kOfxImageEffectContextGenerator))
		return kOfxStatErrFatal;

	/* Output clip */
	gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);

	gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

	/* Parameters */
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

		/* Data file */
	gParamHost->paramDefine(paramSet, kOfxParamTypeString, "dataFile", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Data File");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Path to XML file with chat logs");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);

		/* Render Size */
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble2D, "renderSize", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Render Size");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Target size of the render");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXY);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 640.0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 1, 360.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 100.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 1, 100.0);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 3840.0);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 1, 2160.0);

		/* Font group */
	gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "fontGrp", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Font");

		/* Font: family */
	gParamHost->paramDefine(paramSet, kOfxParamTypeString, "fontFamily", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Family");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Font family (given as-is to Pango rendering)");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "fontGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);

		/* Font: size */
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, "fontSize", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Size");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Font size in pixels");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "fontGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 16.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 8.0);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 64.0);

		/* Color group */
	gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "colorGrp", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Colors");

		/* Color: Background */
	gParamHost->paramDefine(paramSet, kOfxParamTypeRGBA, "bgColor", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Background");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Color for the message background");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "colorGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 0.5);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 1, 0.5);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 2, 0.5);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 3, 0.5);

		/* Color: User name */
	gParamHost->paramDefine(paramSet, kOfxParamTypeRGB, "userColor", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Username");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Color for the message author name");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "colorGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 0.628);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 1, 0.0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 2, 0.0);

		/* Color: Text */
	gParamHost->paramDefine(paramSet, kOfxParamTypeRGB, "textColor", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Text");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Color for the message text content");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "colorGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 0.0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 1, 0.0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 2, 0.0);

		/* Timing group */
	gParamHost->paramDefine(paramSet, kOfxParamTypeGroup, "timingGrp", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Timings");

		/* Timing: Fade In */
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, "fadeInTime", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Fade In");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Time for the scroll-up and fade-in animation (in seconds)");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "timingGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.1);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 10.0);

		/* Timing: Hold */
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, "holdTime", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Hold");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Time for the messages to stay displayed (in seconds)");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "timingGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 15.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 1);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 120.0);

		/* Timing: Fade Out */
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, "fadeOutTime", &props);
	gPropHost->propSetString(props, kOfxPropLabel, 0, "Fade Out");
	gPropHost->propSetString(props, kOfxParamPropHint, 0, "Time for the fade-out animation (in seconds)");
	gPropHost->propSetString(props, kOfxParamPropParent, 0, "timingGrp");
	gPropHost->propSetInt   (props, kOfxParamPropAnimates, 0, 0);
	gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
	gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.1);
	gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 10.0);

	return kOfxStatOK;
}


static OfxStatus
effectGetClipPreferences(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);

	/* We only do RGBA 8 bits */
	gPropHost->propSetString(outArgs, "OfxImageClipPropComponents_Output", 0, kOfxImageComponentRGBA);
	gPropHost->propSetString(outArgs, "OfxImageClipPropDepth_Output", 0, kOfxBitDepthByte);

	/* We're always alpha premultiplied */
	gPropHost->propSetString(outArgs, kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);

	/* We have the same output frame rate as the project */
	gPropHost->propSetDouble(outArgs, kOfxImageEffectPropFrameRate, 0, priv->framerate);

	/* We vary based on time */
	gPropHost->propSetInt(outArgs, kOfxImageEffectFrameVarying, 0, 1);

	return kOfxStatOK;
}


static OfxStatus
effectGetRegionOfDefinition(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);
	OfxRectD rect;

	rect.x1 = 0.0;
	rect.y1 = 0.0;
	rect.x2 = (double) priv->cmr.getWidth();
	rect.y2 = (double) priv->cmr.getHeight();

	gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, &rect.x1);

	return kOfxStatOK;
}


class NoImageEx {};

static void
clipRectI(OfxRectI &r1, OfxRectI &r2)
{
	r1.x1 = std::max(r1.x1, r2.x1);
	r1.x2 = std::min(r1.x2, r2.x2);
	r1.y1 = std::max(r1.y1, r2.y1);
	r1.y2 = std::min(r1.y2, r2.y2);
}

static OfxStatus
effectRender(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	InstanceData *priv = getInstanceData(effect);

	OfxPropertySetHandle outImgProp = 0;
	OfxStatus status = kOfxStatOK;

	try {
		/* Target time */
		OfxTime time;
		gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

		/* Get output image */
		gEffectHost->clipGetImage(priv->outputClip, time, NULL, &outImgProp);

		/* Check it's suitable for us */
		char *str;
		OfxRectI rRender, rOut;

		gPropHost->propGetString(outImgProp, kOfxImageEffectPropPixelDepth, 0, &str);
		if (strcmp(str, kOfxBitDepthByte))
			throw NoImageEx();

		gPropHost->propGetString(outImgProp, kOfxImageEffectPropComponents, 0, &str);
		if (strcmp(str, kOfxImageComponentRGBA))
			throw NoImageEx();

		gPropHost->propGetIntN(outImgProp, kOfxImagePropBounds, 4, &rOut.x1);

		/* Also check render window */
		gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &rRender.x1);

		/* Clip the render window with image bounds */
		clipRectI(rRender, rOut);

		/* Get data pointer and stride */
		uint8_t *out_data;
		int      out_stride;

		gPropHost->propGetPointer(outImgProp, kOfxImagePropData,     0, (void**)&out_data);
		gPropHost->propGetInt    (outImgProp, kOfxImagePropRowBytes, 0, &out_stride);

		/* Clear the render target */
		for (int y=rRender.y1; y<rRender.y2; y++) {
			int ofs = (y * out_stride) + 4 * rRender.x1;
			memset(&out_data[ofs], 0x00, (rRender.x2 - rRender.x1) * 4);
		}

		/* Lock renderer */
		gThreadHost->mutexLock(priv->mutex);

		/* Execute Render */
		int h = priv->cmr.render(time / priv->framerate);

		/* Copy result to the render target */
		if (h > 0) {
			OfxRectI rIn;

			rIn.x1 = 0;
			rIn.x2 = priv->cmr.getWidth();
			rIn.y1 = 0;
			rIn.y2 = h;

			clipRectI(rRender, rIn);

			int            in_stride = priv->cmr.getStride();
			const uint8_t *in_data   = priv->cmr.getData();

			in_data  += (h - rRender.y1 - 1) *  in_stride + rRender.x1 * 4;
			out_data += (    rRender.y1    ) * out_stride + rRender.x1 * 4;

			for (int y=rRender.y1; y<rRender.y2; y++)
			{
				int in_ofs = 0;
				int out_ofs = 0;
				for (int x=rRender.x1; x<rRender.x2; x++) {
					out_data[out_ofs+0] = in_data[in_ofs+2];
					out_data[out_ofs+1] = in_data[in_ofs+1];
					out_data[out_ofs+2] = in_data[in_ofs+0];
					out_data[out_ofs+3] = in_data[in_ofs+3];
					in_ofs  += 4;
					out_ofs += 4;
				}
				in_data  -= in_stride;
				out_data += out_stride;
			}
		}

		/* Unlock renderer */
		gThreadHost->mutexUnLock(priv->mutex);

	} catch(NoImageEx &) {
		/* Missing a required clip, so abort */
		if(!gEffectHost->abort(effect)) {
			status = kOfxStatFailed;
		}
	}

	/* Cleanup */
	if (outImgProp)
		gEffectHost->clipReleaseImage(outImgProp);

	return status;
}


typedef OfxStatus (*handler_func_t)(
	OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs
);

static const struct {
	const char *action;
	handler_func_t handler;
} _apiHandlers[] = {
	{ kOfxActionLoad, effectLoad},
	{ kOfxActionUnload, effectUnload},
	{ kOfxActionCreateInstance, effectCreateInstance },
	{ kOfxActionDestroyInstance, effectDestroyInstance },
	{ kOfxActionInstanceChanged, effectInstanceChanged },
	{ kOfxActionEndInstanceChanged, effectEndInstanceChanged },
	{ kOfxActionDescribe, effectDescribe},
	{ kOfxImageEffectActionDescribeInContext, effectDescribeInContext},
	{ kOfxImageEffectActionGetClipPreferences, effectGetClipPreferences },
	{ kOfxImageEffectActionGetRegionOfDefinition, effectGetRegionOfDefinition },
	{ kOfxImageEffectActionRender, effectRender},
	{ NULL, NULL }
};


/* ------------------------------------------------------------------------- */
/* OpenFX plugin entry points                                                */
/* ------------------------------------------------------------------------- */

static OfxStatus
ofxMain(const char *action,  const void *handle,
        OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	/* Try and catch errors */
	try {
		OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

		for (int i=0; _apiHandlers[i].action; i++)
			if (!strcmp(_apiHandlers[i].action, action))
				return _apiHandlers[i].handler(effect, inArgs, outArgs);

	} catch (const std::bad_alloc&) {
		std::cerr << "[!] OFX Plugin Memory error." << std::endl;
		return kOfxStatErrMemory;
	} catch (const std::exception& e) {
		std::cerr << "[!] OFX Plugin error: " << e.what() << std::endl;
		return kOfxStatErrUnknown;
	} catch (int err) {
		return err;
	} catch ( ... ) {
		std::cerr << "[!] OFX Plugin error" << std::endl;
		return kOfxStatErrUnknown;
	}

	/* Other actions to take the default value */
	return kOfxStatReplyDefault;
}

static void
ofxSetHost(OfxHost *hostStruct)
{
	gHost = hostStruct;
}


/* ------------------------------------------------------------------------- */
/* OpenFX plugin struct and exported func                                    */
/* ------------------------------------------------------------------------- */

static OfxPlugin _plugins[] = {
	[0] = {
		.pluginApi		= kOfxImageEffectPluginApi,
		.apiVersion		= kOfxImageEffectPluginApiVersion,
		.pluginIdentifier	= "be.s47.OfxBBBChat",
		.pluginVersionMajor	= 0,
		.pluginVersionMinor	= 1,
		.setHost		= ofxSetHost,
		.mainEntry		= ofxMain,
	},
};

EXPORT OfxPlugin *
OfxGetPlugin(int nth)
{
	if ((nth >= 0) && (nth < (int)(sizeof(_plugins) / sizeof(_plugins[0]))))
		return &_plugins[nth];
	return NULL;
}

EXPORT int
OfxGetNumberOfPlugins(void)
{
	return sizeof(_plugins) / sizeof(_plugins[0]);
}
