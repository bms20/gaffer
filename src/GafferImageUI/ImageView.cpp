//////////////////////////////////////////////////////////////////////////
//  
//  Copyright (c) 2012, John Haddon. All rights reserved.
//  Copyright (c) 2013, Image Engine Design Inc. All rights reserved.
//  
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//  
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//  
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//  
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//  
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//  
//////////////////////////////////////////////////////////////////////////

#include <math.h>

#include "boost/bind.hpp"
#include "boost/bind/placeholders.hpp"
#include "boost/format.hpp"

#include "OpenEXR/ImathColorAlgo.h"

#include "IECore/FastFloat.h"
#include "IECore/BoxOps.h"
#include "IECore/BoxAlgo.h"

#include "IECoreGL/ToGLTextureConverter.h"
#include "IECoreGL/TextureLoader.h"
#include "IECoreGL/Texture.h"
#include "IECoreGL/ShaderLoader.h"
#include "IECoreGL/Shader.h"
#include "IECoreGL/IECoreGL.h"

#include "Gaffer/Context.h"

#include "GafferUI/Gadget.h"
#include "GafferUI/Style.h"
#include "GafferUI/StandardStyle.h"
#include "GafferUI/Pointer.h"

#include "GafferImage/Format.h"

#include "GafferImageUI/ImageView.h"

using namespace boost;	
using namespace IECoreGL;
using namespace IECore;
using namespace Imath;
using namespace Gaffer;
using namespace GafferUI;
using namespace GafferImage;
using namespace GafferImageUI;

namespace GafferImageUI
{

namespace Detail
{

class ImageViewGadget : public GafferUI::Gadget
{

	public :

		ImageViewGadget(
			IECore::ConstImagePrimitivePtr image,
			GafferImage::ImageStatsPtr imageStats,
			int &channelToView,
			Imath::V2f &mousePos,
			Color4f &sampleColor,
			Color4f &minColor,
			Color4f &maxColor,
			Color4f &averageColor
		)
			:	Gadget( defaultName<ImageViewGadget>() ),
				m_displayBound( image->bound() ),
				m_displayWindow( image->getDisplayWindow() ),
				m_dataWindow( image->getDataWindow() ),
				m_image( image->copy() ),
				m_texture( 0 ),
				m_mousePos( mousePos ),
				m_sampleColor( 0.f),
				m_dragSelecting( false ),
				m_drawSelection( false ),
				m_channelToView( channelToView ),
				m_imageStats( imageStats )
		{
			V3f dataMin( m_dataWindow.min.x, m_dataWindow.min.y, 0.f );
			V3f dataMax( 1.f + m_dataWindow.max.x, 1.f + m_dataWindow.max.y, 0.f );

			
			V3f dispMin( m_displayWindow.min.x, m_displayWindow.min.y, 0.f );
			V3f dispMax( 1.f + m_displayWindow.max.x, 1.f + m_displayWindow.max.y, 0.f );
			V3f dispCenter = ( dispMin + dispMax ) / 2.f;

			const int yOffset = ( m_displayWindow.min.y + m_displayWindow.size().y + 1 ) - m_dataWindow.min.y;
			m_dataBound = Box3f(
				V3f(
					dataMin.x - dispCenter.x,
					( yOffset - ( m_dataWindow.size().y + 1 ) ) - dispCenter.y,
					0.f
				),
				V3f(
					dataMax.x - dispCenter.x,
					( yOffset ) - dispCenter.y,
					0.f
				)
			);

			keyPressSignal().connect( boost::bind( &ImageViewGadget::keyPress, this, ::_1,  ::_2 ) );
			buttonPressSignal().connect( boost::bind( &ImageViewGadget::buttonPress, this, ::_1,  ::_2 ) );
			buttonReleaseSignal().connect( boost::bind( &ImageViewGadget::buttonRelease, this, ::_1,  ::_2 ) );
			dragBeginSignal().connect( boost::bind( &ImageViewGadget::dragBegin, this, ::_1, ::_2 ) );
			dragEnterSignal().connect( boost::bind( &ImageViewGadget::dragEnter, this, ::_1, ::_2 ) );
			dragMoveSignal().connect( boost::bind( &ImageViewGadget::dragMove, this, ::_1, ::_2 ) );
			dragEndSignal().connect( boost::bind( &ImageViewGadget::dragEnd, this, ::_1, ::_2 ) );
			mouseMoveSignal().connect( boost::bind( &ImageViewGadget::mouseMove, this, ::_1, ::_2 ) );

			sampleColor = ImageViewGadget::sampleColor( mousePos );

			// Create some useful structs that we will use to hold information needed
			// to draw the UI elements that display the color readouts to the screen.	
			m_colorUiElements.reserve(4);
			m_colorUiElements.push_back( ColorUiElement( sampleColor ) );
			m_colorUiElements.push_back( ColorUiElement( minColor ) );
			m_colorUiElements.push_back( ColorUiElement( maxColor ) );
			m_colorUiElements.push_back( ColorUiElement( averageColor ) );
			m_colorUiElements[0].name = "RGBA"; // The color under the mouse pointer.
			m_colorUiElements[0].draw = true;
			m_colorUiElements[0].position = V2i( 135, 0 );
			m_colorUiElements[0].hsv = true;
			m_colorUiElements[1].name = "Min"; // The minimum color within a selection.
			m_colorUiElements[1].position = V2i( 135, 19 );
			m_colorUiElements[2].name = "Max"; // The maximum color within a selection.
			m_colorUiElements[2].position = V2i( 385, 19 );
			m_colorUiElements[3].name = "Mean"; // The mean color within a selection.
			m_colorUiElements[3].position = V2i( 635, 19 );
		}

		virtual ~ImageViewGadget()
		{
		};

		virtual Imath::Box3f bound() const
		{
			///\todo: Return an extended bounding box here which includes the infoBox() UI element.
			/// This allows the image to be framed properly when the "f" button has been pressed to
			/// focus the viewer on an image.
			return m_displayBound;
		}
		
	protected :

		static const char *vertexSource()
		{
			static const char *g_vertexSource =
			"void main()"
			"{"
			"	gl_Position = gl_ProjectionMatrix * gl_ModelViewMatrix * gl_Vertex;"
			"	gl_FrontColor = gl_Color;"
			"	gl_BackColor = gl_Color;"
			"	gl_TexCoord[0] = gl_MultiTexCoord0;"
			"}";

			return g_vertexSource;
		}

		static const std::string &fragmentSource()
		{
			static std::string g_fragmentSource;
			if( g_fragmentSource.empty() )
			{
				g_fragmentSource =
				
				"#include \"IECoreGL/FilterAlgo.h\"\n"
				"#include \"IECoreGL/ColorAlgo.h\"\n"

				"uniform sampler2D texture;"
				"uniform int channelToView;\n"

				"#if __VERSION__ >= 330\n"

				"uniform uint ieCoreGLNameIn;\n"
				"layout( location=0 ) out vec4 outColor;\n"
				"layout( location=1 ) out uint ieCoreGLNameOut;\n"
				"#define OUTCOLOR outColor\n"

				"#else\n"

				"#define OUTCOLOR gl_FragColor\n"

				"#endif\n"

				"void main()"
				"{"
				"	OUTCOLOR = texture2D( texture, gl_TexCoord[0].xy );"
				"	OUTCOLOR = vec4( ieLinToSRGB( OUTCOLOR.r ), ieLinToSRGB( OUTCOLOR.g ), ieLinToSRGB( OUTCOLOR.b ), ieLinToSRGB( OUTCOLOR.a ) );"
				"	if( channelToView==1 )"
				"	{"
				"		OUTCOLOR = vec4( OUTCOLOR[0], OUTCOLOR[0], OUTCOLOR[0], 1. );"
				"	}"
				"	else if( channelToView==2 )"
				"	{"
				"		OUTCOLOR = vec4( OUTCOLOR[1], OUTCOLOR[1], OUTCOLOR[1], 1. );"
				"	}"
				"	else if( channelToView==3 )"
				"	{"
				"		OUTCOLOR = vec4( OUTCOLOR[2], OUTCOLOR[2], OUTCOLOR[2], 1. );"
				"	}"
				"	else if( channelToView==4 )"
				"	{"
				"		OUTCOLOR = vec4( OUTCOLOR[3], OUTCOLOR[3], OUTCOLOR[3], 1. );"
				"	}\n"

				"#if __VERSION__ >= 330\n"
				"	ieCoreGLNameOut = ieCoreGLNameIn;\n"
				"#endif\n"
				"}";
			
				if( glslVersion() >= 330 )
				{
					// the __VERSION__ define is a workaround for the fact that cortex's source preprocessing doesn't
					// define it correctly in the same way as the OpenGL shader preprocessing would.
					g_fragmentSource = "#version 330 compatibility\n #define __VERSION__ 330\n\n" + g_fragmentSource;	
				}
			}
			return g_fragmentSource;
		}

		static IECoreGL::ShaderPtr shader()
		{
			static IECoreGL::ShaderPtr g_shader = 0;
			if( !g_shader )
			{
				g_shader = ShaderLoader::defaultShaderLoader()->create( vertexSource(), "", fragmentSource() );
			}
			return g_shader.get();
		}

		void renderImageWindow( const Imath::Box2f &box, const IECoreGL::Texture *texture, int channelToView ) const
		{
			glPushAttrib( GL_COLOR_BUFFER_BIT );

			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

			glEnable( GL_TEXTURE_2D );
			glActiveTexture( GL_TEXTURE0 );
			texture->bind();

			IECoreGL::Shader::SetupPtr setup( new IECoreGL::Shader::Setup( shader() ) );
			setup->addUniformParameter( "texture", texture );

			const IECore::IntDataPtr channelToViewData( new IECore::IntData( channelToView ) );
			setup->addUniformParameter( "channelToView", IECore::staticPointerCast<const IECore::Data>( channelToViewData ) );
			IECoreGL::Shader::Setup::ScopedBinding b( *setup );

			glColor3f( 1.0f, 1.0f, 1.0f );

			glBegin( GL_QUADS );

			glTexCoord2f( 1, 0 );
			glVertex2f( box.max.x, box.min.y );
			glTexCoord2f( 1, 1 );
			glVertex2f( box.max.x, box.max.y );
			glTexCoord2f( 0, 1 );
			glVertex2f( box.min.x, box.max.y );	
			glTexCoord2f( 0, 0 );
			glVertex2f( box.min.x, box.min.y );

			glEnd();

			glPopAttrib();
		}

		bool keyPress( GadgetPtr gadget, const KeyEvent &event )
		{
			int channel = All;
			if( event.key=="R" )
			{
				channel = Red;
			}
			else if( event.key=="G" )
			{
				channel = Green;
			}
			else if( event.key=="B" )
			{
				channel = Blue;
			}
			else if( event.key=="A" )
			{
				channel = Alpha;
			}
			else
			{
				return false;
			}

			if( channel == m_channelToView )
			{
				m_channelToView = All;
			}
			else
			{
				m_channelToView = channel;
			}

			renderRequestSignal()( this );

			return true;
		}

		/// Returns the data window of the image in raster space.
		inline Box2f dataRasterBox() const
		{
			const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
			return Box2f(
					viewportGadget->gadgetToRasterSpace( V3f( m_dataBound.min.x, m_dataBound.min.y, 0.), this ),
					viewportGadget->gadgetToRasterSpace( V3f( m_dataBound.max.x, m_dataBound.max.y, 0.), this )
					);
		}

		/// Returns the display window of the image in raster space.
		inline Box2f displayRasterBox() const
		{
			const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
			return Box2f(
					viewportGadget->gadgetToRasterSpace( V3f( m_displayBound.min.x, m_displayBound.min.y, 0.), this ),
					viewportGadget->gadgetToRasterSpace( V3f( m_displayBound.max.x, m_displayBound.max.y, 0.), this )
					);
		}

		/// Transforms and returns a point from raster space to display space.
		V2f rasterToDisplaySpace( const V2f &point ) const
		{
			Box2f dispRasterBox( displayRasterBox() );
			V2f wh( ( dispRasterBox.max.x - dispRasterBox.min.x ) + 1.f, ( dispRasterBox.min.y - dispRasterBox.max.y ) + 1.f );
			V2f t( ( point[0] - dispRasterBox.min.x ) / wh[0], ( point[1] - dispRasterBox.max.y ) / wh[1] );
			return V2f(
				float( m_displayWindow.min.x ) + t.x * ( m_displayWindow.size().x + 1.f ),
				float( m_displayWindow.max.y + 1.f ) - ( t.y * ( m_displayWindow.size().y + 1.f ) )
			);
		}

		Box2f rasterToDisplaySpace( const Box2f &box ) const
		{
			return Box2f( rasterToDisplaySpace( box.min ), rasterToDisplaySpace( box.max ) );
		}

		/// Transforms and returns a point from raster to display space.
		V2f gadgetToDisplaySpace( const V3f &point ) const
		{
			const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
			V2i pointRasterPos( viewportGadget->gadgetToRasterSpace( point, this ) );
			return rasterToDisplaySpace( V2f( floorf( pointRasterPos.x ), floorf( pointRasterPos.y ) ) );
		}

		Box2f gadgetToDisplaySpace( const Box3f &box ) const
		{
			return Box2f( gadgetToDisplaySpace( box.min ), gadgetToDisplaySpace( box.max ) );
		}

		/// Samples a color from the image.
		///\todo: This method currently samples a pixel from the ImagePrimitive and this means that the sampled colour is the result of the image preprocessor.
		/// Instead we should really be sampling from the input image plug so that the user is presented with raw colour data.
		Color4f sampleColor( const V2f &point ) const
		{
			V2i samplePos(
				fastFloatRound( point.x - .5 ) - m_dataWindow.min.x,
				( ( m_displayWindow.max.y - fastFloatRound( point.y - .5 ) ) - m_dataWindow.min.y )
			);
			
			if ( samplePos.x < 0 || samplePos.y < 0 || samplePos.x > m_dataWindow.size().x || samplePos.y > m_dataWindow.size().y )
			{
				return Color4f( 0.f, 0.f, 0.f, 0.f );
			}

			Color4f color;
			std::vector<std::string> channelNames;
			m_image->channelNames( channelNames );
			std::string channel[4] = { "R", "G", "B", "A" };
			for ( int c = 0; c < 4; ++c )
			{
				if ( std::find( channelNames.begin(), channelNames.end(), channel[c] ) != channelNames.end() )
				{
					const std::vector<float> &channelData = m_image->getChannel<float>( channel[c] )->readable();
					color[c] = channelData[ samplePos.y * ( m_dataWindow.size().x + 1 ) + samplePos.x ];
				}
				else
				{
					color[c] = 0.f;
				}
			}

			return color;
		};

		bool buttonRelease( GadgetPtr gadget, const ButtonEvent &event )
		{
			return false;
		}

		bool buttonPress( GadgetPtr gadget, const ButtonEvent &event )
		{
			if( event.buttons != ButtonEvent::Left )
			{
				return false;
			}

			V3f mouseGadgetPos( event.line.p0.x, event.line.p0.y, 0.f );
			m_mousePos = gadgetToDisplaySpace( mouseGadgetPos );

			/// Check to see if the user is clicking on one of the swatches...	
			if ( ( event.modifiers & ModifiableEvent::Shift ) == false )
			{
				const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
				V2f mouseRasterPos( viewportGadget->gadgetToRasterSpace( mouseGadgetPos, this ) );
				for( unsigned int i = 0; i < 4; ++i )
				{
					V2f origin( m_colorUiElements[i].position + infoBox().min );
					Box2f swatchBox( m_colorUiElements[i].swatchBox.min + origin, m_colorUiElements[i].swatchBox.max + origin );
					if( boxIntersects( swatchBox, mouseRasterPos ) )
					{
						m_sampleColor = *m_colorUiElements[i].color;
						renderRequestSignal()( this );
						return true;
					}
				}
			}

			m_drawSelection = m_colorUiElements[1].draw = m_colorUiElements[2].draw = m_colorUiElements[3].draw = false;
			*m_colorUiElements[0].color = m_sampleColor = sampleColor( m_mousePos );
			renderRequestSignal()( this );

			return true;
		}

		bool mouseMove( GadgetPtr gadget, const ButtonEvent &event )
		{
			m_mousePos = gadgetToDisplaySpace( V3f( event.line.p0.x, event.line.p0.y, 0 ) );
			*m_colorUiElements[0].color = sampleColor( m_mousePos );
			renderRequestSignal()( this );
			return true;
		}

		bool dragEnter( GadgetPtr gadget, const DragDropEvent &event )
		{
			if ( m_dragSelecting )
			{
				return true;
			}
			return event.sourceGadget == this && event.data == this;
		}

		bool dragMove( GadgetPtr gadget, const DragDropEvent &event )
		{
			m_lastDragPosition = event.line.p1;
			
			// Update the selection box.
			if ( m_dragSelecting )
			{				
				m_drawSelection = m_colorUiElements[1].draw = m_colorUiElements[2].draw = m_colorUiElements[3].draw = true;
				
				Box3f selectionBox;
				selectionBox.extendBy( m_dragStartPosition );
				selectionBox.extendBy( m_lastDragPosition );
				setSelectionArea( selectionBox );

				/// Set the region of interest on our internal ImageStats node and
				/// get the min, max and average value of the image.	
				const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
				Box2f roif(
					V2f( viewportGadget->gadgetToRasterSpace( m_sampleWindow.min, this ) ),
					V2f( viewportGadget->gadgetToRasterSpace( m_sampleWindow.max, this ) )
				);
				roif = rasterToDisplaySpace( roif );

				Box2i roi(
					V2i(
						fastFloatRound( roif.min.x ),
						fastFloatRound( roif.min.y )
					),
					V2i(
						fastFloatRound( roif.max.x ) - 1,
						fastFloatRound( roif.max.y ) - 1
					)
				);
				m_imageStats->regionOfInterestPlug()->setValue( roi );
		
				*m_colorUiElements[1].color = m_imageStats->minPlug()->getValue();
				*m_colorUiElements[2].color = m_imageStats->maxPlug()->getValue();
				*m_colorUiElements[3].color = m_imageStats->averagePlug()->getValue();
			}

			m_mousePos = gadgetToDisplaySpace( V3f( event.line.p0.x, event.line.p0.y, 0 ) );
			*m_colorUiElements[0].color = sampleColor( m_mousePos );
			renderRequestSignal()( this );
			return true;
		}

		bool dragEnd( GadgetPtr gadget, const DragDropEvent &event )
		{
			Pointer::set( 0 );
			if( !m_dragSelecting )
			{
				return false;
			}
			m_dragSelecting = false;
			
			renderRequestSignal()( this );
			return true;
		}

		IECore::RunTimeTypedPtr dragBegin( GadgetPtr gadget, const DragDropEvent &event )
		{
			if ( event.modifiers & ModifiableEvent::Shift )
			{
				m_dragSelecting = true;
				m_dragStartPosition = m_lastDragPosition = event.line.p0;
				return this;
			}

			Pointer::setFromFile( "rgba.png" );
			return new Color4fData( m_sampleColor );
		}
	
		void setSelectionArea( Box3f selectionBox )
		{
			// Get the bounds of the selection.
			selectionBox = boxIntersection( selectionBox, m_displayBound );

			if ( selectionBox.size().x < 0. )
			{
				selectionBox.max.x = selectionBox.min.x;
			}

			if ( selectionBox.size().y < 0. )
			{
				selectionBox.max.y = selectionBox.min.y;
			}

			selectionBox.min.x = floorf( selectionBox.min.x );
			selectionBox.min.y = floorf( selectionBox.min.y );
			selectionBox.max.x = ceilf( selectionBox.max.x );
			selectionBox.max.y = ceilf( selectionBox.max.y );
			
			// Save the box in gadget space.
			m_sampleWindow = selectionBox;
		}

		void drawWindow( Box2f &rasterWindow,  const Style *style ) const
		{
			V2f minPt( rasterToDisplaySpace( rasterWindow.min ) );
			V2f maxPt( rasterToDisplaySpace( rasterWindow.max ) );
			drawWindow( rasterWindow, minPt, maxPt, style );
		}

		void drawWindow( Box2f &rasterWindow, const V2f &bottomLeftPoint, const V2f &topRightPoint, const Style *style ) const
		{
			V2f minPt( rasterToDisplaySpace( rasterWindow.min ) );
			V2f maxPt( rasterToDisplaySpace( rasterWindow.max ) );
				
			std::string rasterWindowMinStr = std::string( boost::str( boost::format( "(%d, %d)" ) % fastFloatRound( bottomLeftPoint.x ) % fastFloatRound( bottomLeftPoint.y ) ) );
			std::string rasterWindowMaxStr = std::string( boost::str( boost::format( "(%d, %d)" ) % fastFloatRound( topRightPoint.x ) % fastFloatRound( topRightPoint.y ) ) );
				
			// Draw the box around the data window.
			style->renderRectangle( rasterWindow );
			
			glTranslatef( rasterWindow.max.x+5, rasterWindow.max.y-5, 0.f ); 
			glScalef( 10.f, -10.f, 1.f );
			style->renderText( Style::LabelText, rasterWindowMaxStr );
				
			glLoadIdentity();
			glTranslatef( rasterWindow.min.x+5, rasterWindow.min.y+10, 0.f ); 
			glScalef( 10.f, -10.f, 1.f );
			
			style->renderText( Style::LabelText, rasterWindowMinStr );
			glLoadIdentity();
		}

		Box2f infoBox() const
		{
			const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
			V2i viewportWH( viewportGadget->getViewport() );
			Box2f infoBox(
					V2f( 0.f, viewportWH.y-20),
					viewportWH
				);
			if ( m_drawSelection )
			{
				infoBox.min.y = viewportWH.y-40;
			}
			return infoBox;
		}

		virtual void doRender( const Style *style ) const
		{

			if( !m_texture )
			{
				// convert image to texture
				ToGLTextureConverterPtr converter = new ToGLTextureConverter( staticPointerCast<const ImagePrimitive>( m_image ), true );
				m_texture = IECore::runTimeCast<IECoreGL::Texture>( converter->convert() );

				{
					Texture::ScopedBinding scope( *m_texture );
					glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
					glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
				}
			}

			// Transform them to Raster Space
			///\todo: The RasterScope class transforms Gadgets into a space where coordinate (0, 0) is in the top left corner.
			/// If we are rasterizing gadgets in 2D then we want (0, 0) to be in the bottom left corner. Perhaps we should write
			/// a new Scope class to transform to the appropriate 2D space or add a flag to the RasterScope class to flip the Y axis.
			/// Because of this issue we have to flip the Y axis in a few places such as when we scale the text.
			const ViewportGadget *viewportGadget = ancestor<ViewportGadget>();
			Box2f dispRasterBox( displayRasterBox() );
			Box2f dataRasterBox( ImageViewGadget::dataRasterBox() );

			{
				ViewportGadget::RasterScope rasterScope( viewportGadget );

				// Draw the display window background.
				Color4f color( 0.0f, .0f, .0f, 1.f );
				glColor( color );
				style->renderSolidRectangle( dispRasterBox );
			}

			// Draw the image data.
			{
				// Get the bounds of the data window in Gadget space.
				Box2f b( V2f( m_dataBound.min.x, m_dataBound.min.y ), V2f( m_dataBound.max.x, m_dataBound.max.y ) );
				renderImageWindow( b, (const Texture *)m_texture.get(), m_channelToView );
			}

			ViewportGadget::RasterScope rasterScope( viewportGadget );

			// Mask the data window where it doesn't overlap the display window.
			if ( m_dataWindow != m_displayWindow )
			{
				const Box2f& c = dispRasterBox;
				Box2f b = dataRasterBox;

				///\todo We should query the raised color of the current style here and use it but the current design won't allow us. For now it is hard-coded...
				glColor( Color4f( .29804, .29804, .29804, .90 ) );

				if ( b.min.x <= c.min.x )
				{
					Box2f l(b);
					l.max.x = c.min.x;
					b.min.x = c.min.x;
					style->renderSolidRectangle( l );
				}

				if ( b.max.x >= c.max.x )
				{
					Box2f r(b);
					r.min.x = c.max.x;
					b.max.x = c.max.x;
					style->renderSolidRectangle( r );
				}

				if ( b.max.y <= c.max.y )
				{
					Box2f l(b);
					l.min.y = c.max.y;
					b.max.y = c.max.y;
					style->renderSolidRectangle( l );
				}

				if ( b.min.y >= c.min.y )
				{
					Box2f r(b);
					r.max.y = c.min.y;
					b.min.y = c.min.y;
					style->renderSolidRectangle( r );
				}
			}

			// Draw the box around the display window.
			Color4f color( .1f, .1f, .1f, 1.f );
			glColor( color );
			style->renderRectangle( dispRasterBox );
			glLoadIdentity();

			// Draw the display window text.
			glTranslatef( dispRasterBox.min.x+5, dispRasterBox.min.y+10, 0.f );
			glScalef( 10.f, -10.f, 1.f );

			///\todo: How do we handle looking up the format here when we have a pixel aspect other than 1?
			/// Does the IECore::ImagePrimitive have a pixel aspect?
			GafferImage::Format f( m_displayWindow.size().x+1, m_displayWindow.size().y+1, 1. );
			std::string formatName( Format::formatName( f ) ) ;

			style->renderText( Style::LabelText, formatName );
			glLoadIdentity();

			// Draw the data window if it is different to the display window.
			if ( m_dataWindow != m_displayWindow && m_dataWindow.hasVolume() )
			{
				color = Color4f( .2f, .2f, .2f, 1.f );
				glColor( color );
				drawWindow( dataRasterBox, m_dataWindow.min, m_dataWindow.max, style );
			}

			/// Draw the selection window.
			if( m_drawSelection )
			{
				Box2f rasterBox(
					V2i( viewportGadget->gadgetToRasterSpace( m_sampleWindow.min, this ) ),
					V2i( viewportGadget->gadgetToRasterSpace( m_sampleWindow.max, this ) )
				);

				color = Color4f( 1.f, 0.f, 1.f, 1.f );
				glColor( color );
				drawWindow( rasterBox, style );
			}

			// Draw the color information bar.	
			Imath::Box2f infoBarBox( infoBox() );
			color = Color4f( 0.f, 0.f, 0.f, 1.f );
			glColor( color );
			style->renderSolidRectangle( infoBarBox );
			glColor( Color4f( .29804, .29804, .29804, .90 ) );
			style->renderRectangle( infoBarBox );
			
			// Draw the channel mask icon.
			Imath::Box2f iconBox( V2f( 10.f, 5.f ) + infoBarBox.min, V2f( 20.f, 15.f ) + infoBarBox.min );
			if( m_channelToView == All )
			{
				float sliceWidth = iconBox.size().x / 3.f;
				Imath::Box2f iconSlice( iconBox.min, Imath::V2f( sliceWidth + iconBox.min.x, iconBox.max.y ) );
				glColor( Color4f( 1., 0., 0., 1. ) );
				style->renderSolidRectangle( iconSlice );

				iconSlice.min.x += sliceWidth;
				iconSlice.max.x += sliceWidth;
				glColor( Color4f( 0., 1., 0., 1. ) );
				style->renderSolidRectangle( iconSlice );
				
				iconSlice.min.x += sliceWidth;
				iconSlice.max.x += sliceWidth;
				glColor( Color4f( 0., 0., 1., 1. ) );
				style->renderSolidRectangle( iconSlice );
			}
			else if( m_channelToView == Red )
			{
				glColor( Color4f( 1., 0., 0., 1. ) );
				style->renderSolidRectangle( iconBox );
			}
			else if( m_channelToView == Green )
			{
				glColor( Color4f( 0., 1., 0., 1. ) );
				style->renderSolidRectangle( iconBox );
			}
			else if( m_channelToView == Blue )
			{
				glColor( Color4f( 0., 0., 1., 1. ) );
				style->renderSolidRectangle( iconBox );
			}
			else if( m_channelToView == Alpha )
			{
				glColor( Color4f( 1., 1., 1., 1. ) );
				style->renderSolidRectangle( iconBox );
			}

			glColor( Color4f( .29804, .29804, .29804, .90 ) );
			style->renderRectangle( infoBarBox );
			glLoadIdentity();

			// Draw the mouse's XY position.
			glTranslatef( infoBarBox.min.x+30, infoBarBox.min.y+14, 0.f );
			glScalef( 10.f, -10.f, 1.f );
			std::string mousePosStr = std::string( boost::str( boost::format( "XY: %d, %d" ) % fastFloatRound( m_mousePos.x - .5 ) % fastFloatRound( m_mousePos.y - .5 ) ) );
			style->renderText( Style::LabelText, mousePosStr );
			glLoadIdentity();
			
			for( unsigned int i = 0; i < m_colorUiElements.size(); ++i )
			{
				if( m_colorUiElements[i].draw )
				{
					V2f origin( m_colorUiElements[i].position + infoBarBox.min );
					
					// Render a little swatch of the color.
					Box2f swatchBox( m_colorUiElements[i].swatchBox );
					swatchBox.min += origin;
					swatchBox.max += origin;

					glColor( Color4f( 0.f, 0.f, 0.f, 1.f ) );
					style->renderSolidRectangle( swatchBox );
					glColor( Color4f( 1.f ) );
					style->renderSolidRectangle( Box2f( swatchBox.min, swatchBox.min + ( swatchBox.size() / V2f( 2. ) ) ) );
					style->renderSolidRectangle( Box2f( swatchBox.min + ( swatchBox.size() / V2f( 2. ) ), swatchBox.max ) );
					glColor( *m_colorUiElements[i].color );
					style->renderSolidRectangle( swatchBox );
					glColor( Color4f( .29804, .29804, .29804, .90 ) );
					style->renderRectangle( swatchBox );
					
					// Render the text next to the swatch.
					V2f textPos( swatchBox.max.x + 5.f, origin.y + 14.f );
					glTranslatef( textPos.x, textPos.y, 0.f );
					glScalef( 10.f, -10.f, 1.f );
					std::string infoStr = std::string(
							boost::str(
								boost::format( "%s: %.3f, %.3f, %.3f, %.3f" )
								% m_colorUiElements[i].name.c_str()
								% (*m_colorUiElements[i].color)[0]
								% (*m_colorUiElements[i].color)[1]
								% (*m_colorUiElements[i].color)[2]
								% (*m_colorUiElements[i].color)[3]
								)
							);
					
					if( m_colorUiElements[i].hsv )
					{
						const Color4f hsv = Imath::rgb2hsv( *m_colorUiElements[i].color );
						infoStr += boost::str(
							boost::format( "  HSV: %.3f, %.3f, %.3f" ) % hsv[0] % hsv[1] % hsv[2]
						);
					}
							
					style->renderText( Style::LabelText, infoStr );
					glLoadIdentity();
				}
			}
		}

	private :
		
		enum ChannelToView
		{
			All = 0,
			Red = 1,
			Green = 2,
			Blue = 3,
			Alpha = 4
		};

		/// A simple struct that we use to hold the information required to draw a UI element that displays information about a color.
		struct ColorUiElement
		{
			ColorUiElement( Color4f &swatchColor ): draw( false ), name( "RGBA" ), color( &swatchColor ), position( 0.f ), swatchBox( V2f( 0.f, 5.f ), V2f( 10.f, 15.f ) ), hsv( false ) {}
			bool draw;
			std::string name;
			Color4f *color;
			Imath::V2f position;
			Imath::Box2f swatchBox;
			bool hsv;
		};

		Imath::Box3f m_displayBound;
		Imath::Box3f m_dataBound;
		Imath::Box2i m_displayWindow;
		Imath::Box2i m_dataWindow;
		ConstImagePrimitivePtr m_image;
		mutable ConstTexturePtr m_texture;

		Imath::V2f &m_mousePos;
		Imath::V3f m_dragStartPosition;
		Imath::V3f m_lastDragPosition;
		Color4f m_sampleColor;
		bool m_dragSelecting;
		bool m_drawSelection;
		int &m_channelToView;

		Imath::Box3f m_sampleWindow;
		GafferImage::ImageStatsPtr m_imageStats;
		std::vector<ColorUiElement> m_colorUiElements;
};

IE_CORE_DECLAREPTR( ImageViewGadget );

}; // namespace Detail

}; // namespace GafferImageUI

//////////////////////////////////////////////////////////////////////////
/// Implementation of ImageView
//////////////////////////////////////////////////////////////////////////

IE_CORE_DEFINERUNTIMETYPED( ImageView );

ImageView::ViewDescription<ImageView> ImageView::g_viewDescription( GafferImage::ImagePlug::staticTypeId() );

ImageView::ImageView( const std::string &name )
	:	View( name, new GafferImage::ImagePlug() ),
		m_channelToView(0),
		m_mousePos(0.),
		m_sampleColor(0.),
		m_minColor(0.),
		m_maxColor(0.),
		m_averageColor(0.)
{
	// Create an internal ImageStats node 
	addChild( new GafferImage::ImageStats( "imageStats" ) );
}

ImageView::ImageView( const std::string &name, Gaffer::PlugPtr input )
	:	View( name, input ),
		m_channelToView(0),
		m_mousePos( 0. ),
		m_sampleColor(0.),
		m_minColor(0.),
		m_maxColor(0.),
		m_averageColor(0.)
{
	// Create an internal ImageStats node 
	addChild( new GafferImage::ImageStats( "imageStats" ) );
}

ImageView::~ImageView()
{
}

GafferImage::ImageStats *ImageView::imageStatsNode()
{
	return getChild<ImageStats>( "imageStats" );
}

const GafferImage::ImageStats *ImageView::imageStatsNode() const
{
	return getChild<ImageStats>( "imageStats" );
}

void ImageView::update()
{
	IECore::ConstImagePrimitivePtr image = 0;
	{
		Context::Scope context( getContext() );
		ImagePlug *imagePlug = preprocessedInPlug<ImagePlug>();
		if( !imagePlug )
		{
			throw IECore::Exception( "ImageView::preprocessedInPlug() is not an ImagePlug" );
		}
		image = imagePlug->image();
	}

	if( image )
	{
		GafferImage::ImagePlug *imagePlug( inPlug<ImagePlug>() ? inPlug<ImagePlug>() : preprocessedInPlug<ImagePlug>() );
		if( !imagePlug )
		{
			throw IECore::Exception("ImageView: Failed to find an input ImagePlug");
		}

		imageStatsNode()->inPlug()->setInput( imagePlug );
		imageStatsNode()->channelsPlug()->setInput( imagePlug->channelNamesPlug() );

		Detail::ImageViewGadgetPtr imageViewGadget = new Detail::ImageViewGadget( image, imageStatsNode(), m_channelToView, m_mousePos, m_sampleColor, m_minColor, m_maxColor, m_averageColor );
		bool hadChild = viewportGadget()->getChild<Gadget>();
		viewportGadget()->setChild( imageViewGadget );
		if( !hadChild )
		{
			viewportGadget()->frame( imageViewGadget->bound() );
		}
	}
	else
	{
		viewportGadget()->setChild( 0 );	
	}
}

