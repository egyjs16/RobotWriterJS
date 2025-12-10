#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <windows.h>
#include "rs232.h"
#include "serial.h"

#define bdrate 115200               /* 115200 baud */
#define MAX_POINTS 64
#define MAX_GLYPHS 128
#define MAX_TEXT_LENGTH 1024
#define MAX_TEXT_GLYPHS 128
#define MAX_LINE_LENGTH 1024

// Struct definitions
struct strokePoint
{
    float x;
    float y;
    int penState; // S0 for active, S1000 for inactive
};

struct glyph
{
    int charCode;
    int pointCount;
    struct strokePoint points[MAX_POINTS];
};

struct fontData
{
    struct glyph glyphs[MAX_GLYPHS];
    int gCount;
};

struct rawGlyphData
{
    float x;
    float y;
    int penState;
};

// Function prototypes
void SendCommands (char *buffer );
float getTextHeight();
int loadFontData(const char *fontDataFile, struct fontData *f);
void scaleFontData(struct fontData *f, float scaleFactor);
int loadTextFile(const char *testFile, char *buffer, int maxLength);
const struct glyph* searchForGlyphData(const struct fontData *font, int charCode);
int convertTextToGlyhs(const struct fontData *font, const char *text, struct glyph *textGlyphs, int maxGlyphs);
void shiftGlyphArray(struct glyph *textGlyphs, int gCount, float shift);
int makeRawGlyphCoordData(const struct glyph *textGlyphs, int gCount, float (*rawGlyphs)[3]);
int makeGCode(const float (*rawGlyphData)[3], int totalLines, char ***gCodeLines);


int main()
{
    //char mode[]= {'8','N','1',0};
    char buffer[100];

    // If we cannot open the port then give up immediately
    if ( CanRS232PortBeOpened() == -1 )
    {
        printf ("\nUnable to open the COM port (specified in serial.h) ");
        exit (0);
    }

    // Text height aquisition 
    float textHeightUnscaled = getTextHeight();
    // Scale user input height to actual value required to be used by program
    float textHeight;
    textHeight = textHeightUnscaled/18;
    printf("Scaled height is %f\n", textHeight);

    // Font data loading
    struct fontData SSFont;
    if (loadFontData("SingleStrokeFont.txt", &SSFont) != 0)
    {
        printf("Failed to load font data\n");
        
        return 1;
    }

    printf("Loaded font data\n");

    // Font data scaling
    scaleFontData(&SSFont, textHeight);
    
    // Text to be written file loading
    char text[MAX_TEXT_LENGTH];
    if (loadTextFile("test.txt", text, MAX_TEXT_LENGTH) != 0)
    {
        printf("Text file reading failed\n");
        return 1;
    }

    printf("Text to be written by Drawing Robot - %s\n", text);

    // Convert stored sentence to an array of glyphs
    struct glyph textGlyphs[MAX_TEXT_GLYPHS];
    int gCount = convertTextToGlyhs(&SSFont, text, textGlyphs, MAX_TEXT_GLYPHS);
    printf("no of glyphs in text - %d\n", gCount);

    // Shift glyph data so that letters are not all ocupying same space
    shiftGlyphArray(textGlyphs, gCount, textHeightUnscaled);

    // Prepare array that contains text to be written for conversion to gcode
    // Remove any indexes on textGlyphs so just x, y and penState remain
    int totalLinesGuess  = gCount * MAX_POINTS;
    float (*rawGlyphData)[3] = malloc(totalLinesGuess * sizeof(*rawGlyphData));
    if (!rawGlyphData)
    {
        printf("mem alloc failed for rawGlyphData\n");
        return 1;
    }
   
    int totalLines = makeRawGlyphCoordData(textGlyphs, gCount, rawGlyphData);

    char **gCodeLines = NULL;
    int gCodeLinesCount = makeGCode(rawGlyphData, totalLines, &gCodeLines);
    if (gCodeLinesCount < 0)
    {
        printf("failed to generate gcode\n");
        free(rawGlyphData);
        return 1;
    }

    // Gcode fully written and stored ready to send line by line

    // Time to wake up the robot
    printf ("\nAbout to wake up the robot\n");

    // We do this by sending a new-line
    sprintf (buffer, "\n");
     // printf ("Buffer to send: %s", buffer); // For diagnostic purposes only, normally comment out
    PrintBuffer (&buffer[0]);
    Sleep(100);

    // This is a special case - we wait  until we see a dollar ($)
    WaitForDollar();

    printf ("\nThe robot is now ready to draw\n");

    //These commands get the robot into 'ready to draw mode' and need to be sent before any writing commands
    sprintf (buffer, "G1 X0 Y0 F1000\n");
    SendCommands(buffer);
    sprintf (buffer, "M3\n");
    SendCommands(buffer);
    sprintf (buffer, "S0\n");
    SendCommands(buffer);

    // Send gCode lines
    for (int i = 0; i < gCodeLinesCount; i++)
    {
        sprintf(buffer, "%s\n", gCodeLines[i]);
        SendCommands(buffer);
    }

    for (int i = 0; i < gCodeLinesCount; i++)
    {
        free(gCodeLines[i]);
    }

    free(gCodeLines);
    free(rawGlyphData);

    // Before we exit the program we need to close the COM port
    CloseRS232Port();
    printf("Com port now closed\n");

    return (0);
}

// Send the data to the robot - note in 'PC' mode you need to hit space twice
// as the dummy 'WaitForReply' has a getch() within the function.
void SendCommands (char *buffer )
{
    printf ("Buffer to send: %s", buffer); // For diagnostic purposes only, normally comment out
    PrintBuffer (&buffer[0]);
    WaitForReply();
    Sleep(100); // Can omit this when using the writing robot but has minimal effect
    // getch(); // Omit this once basic testing with emulator has taken place
}

float getTextHeight()
{
    // Get user input for desired text height
    float textHeightUnscaled;
    while(1)
    {
        printf("Enter a value between 4 and 10 (mm) for the output text height - ");
        scanf("%f", &textHeightUnscaled);

        if (textHeightUnscaled >= 4 && textHeightUnscaled <= 10)
        {
          printf("Text height set to %f\n", textHeightUnscaled);
          break;
        }
        else
        {
            printf("Input outside of range\n");
        }
    }
    
    return textHeightUnscaled;
}

int loadFontData(const char *fontDataFile, struct fontData *f)
{
    FILE *fPtr = fopen(fontDataFile, "r");
    if (!fPtr)
    {
        printf("Error opening file %s\n", fontDataFile);
        return 1;
    }

    f -> gCount = 0;

    int marker, charCode, pointCount;
    
    while (fscanf(fPtr, "%d", &marker) == 1)
    {
        if (marker == 999)
        {
            if (fscanf(fPtr, "%d %d", &charCode, &pointCount) != 2)
                break;

            if (f -> gCount >= MAX_GLYPHS)
            {
                printf("Maximum number of glyphs exceeded\n");
                break;
            }

        struct glyph *g = &f -> glyphs[f -> gCount];
        g -> charCode =charCode;
        g -> pointCount = pointCount;

        for (int i = 0; i < pointCount && i < MAX_POINTS; i++)
        {
            fscanf(fPtr, "%f %f %d", &g -> points[i].x, &g -> points[i].y, &g -> points[i].penState);
        }

        f -> gCount++;
        }
    }

    fclose(fPtr);
    
    return 0;
}

void scaleFontData(struct fontData *f, float scaleFactor)
{
    for (int g = 0; g < f -> gCount; g++)
    {
        struct glyph *gl = &f -> glyphs[g];

        for (int p = 0; p < gl -> pointCount; p ++)
        {
            gl -> points[p].x = gl -> points[p].x * scaleFactor;
            gl -> points[p].y = gl -> points[p].y * scaleFactor;
            gl -> points[p].penState = gl -> points[p].penState * 1000;
        }
    }
}

int loadTextFile(const char *testFile, char *buffer, int maxLength)
{
    FILE *fPtr = fopen(testFile, "r");
    if(!fPtr)
    {
        printf("Error opening file\n");
        return 1;
    }

    buffer[0] = '\0';

    char line[256];
    while (fgets(line, sizeof(line), fPtr) != NULL)
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[len - 1] = '\0';
            len--;
        }

        if (buffer[0] != '\0')
        {
            strncat(buffer, " ", (size_t)maxLength - strlen(buffer) - 1);
        }

        strncat(buffer, line, (size_t)maxLength - strlen(buffer) - 1);
    }

    fclose(fPtr);
    return 0;
}

const struct glyph* searchForGlyphData(const struct fontData *font, int charCode)
{
    for (int i = 0; i < font -> gCount; i++)
    {
        if (font -> glyphs[i].charCode == charCode)
            return &font -> glyphs[i];
    }
    return NULL;
}

int convertTextToGlyhs(const struct fontData *font, const char *text, struct glyph *textGlyphs, int maxGlyphs)
{
    int count = 0;
    for (int i = 0; text[i] != '\0' && count < maxGlyphs; i++)
    {
        int charCode = (unsigned char)text[i];
        const struct glyph *g = searchForGlyphData(font, charCode);

        if (g != NULL)
        {
            textGlyphs[count++] = *g;
        }
    }
    
    return count;
}

void shiftGlyphArray(struct glyph *textGlyphs, int gCount, float shift)
{
    for (int i = 0; i < gCount; i++)
    {
        float xOffset = shift * i;

        for (int j = 0; j <textGlyphs[i].pointCount && j < MAX_POINTS; j++)
        {
            textGlyphs[i].points[j].x += xOffset;

            while (textGlyphs[i].points[j].x > 100)
            {
                textGlyphs[i].points[j].x = textGlyphs[i].points[j].x - 100;
                textGlyphs[i].points[j].y = textGlyphs[i].points[j].y - (shift + 5);
            }
        }
    }
}

int makeRawGlyphCoordData(const struct glyph *textGlyphs, int gCount, float (*rawGlyphs)[3])
{
    int index = 0;
    for (int i = 0; i < gCount; i++)
    {
        for (int j = 0; j < textGlyphs[i].pointCount; j++)
        {
            rawGlyphs[index][0] = textGlyphs[i].points[j].x;
            rawGlyphs[index][1] = textGlyphs[i].points[j].y;
            rawGlyphs[index][2] = (float)textGlyphs[i].points[j].penState;
            index++;
        }
    }

    return index;
}

int makeGCode(const float (*rawGlyphData)[3], int totalLines, char ***gCodeLines)
{
    int gCount = (totalLines * 2) + 2;
    *gCodeLines = (char **)malloc(sizeof(char *) * gCount);
    if (!(*gCodeLines))
    {
        return 1;
    }

    for (int i = 0; i < gCount; i++)
    {
        (*gCodeLines)[i] = (char *)malloc(128);
        if (!(*gCodeLines)[i])
        {
            for (int j = 0; j < i; j++)
            {
                free((*gCodeLines)[j]);
            }
            
            free(*gCodeLines);

            return 1;
        }
    }

    // First line
    snprintf((*gCodeLines)[0],128, "G0 F1000 X0 Y0 S1000");

    // Main block of lines
    int lineNo = 1;
    for (int i = 0; i < totalLines; i++)
    {
        snprintf((*gCodeLines)[lineNo], 128, "G1 X%f Y%f",
                 rawGlyphData[i][0],
                 rawGlyphData[i][1]);
        lineNo++;

        snprintf((*gCodeLines)[lineNo],128, "S%f",
                 rawGlyphData[i][2]);
        lineNo++;
    }

    // Last line
    snprintf((*gCodeLines)[gCount - 1],128, "G1 X0 Y0 S1000");

    return gCount;
}