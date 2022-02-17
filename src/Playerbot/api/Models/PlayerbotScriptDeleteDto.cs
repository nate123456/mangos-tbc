namespace Playerbot.Api.Models;

public class PlayerbotScriptDeleteDto
{
    public int AccountId { get; set; }
    public IEnumerable<string> ScriptNames { get; set; }
}
