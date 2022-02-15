namespace Playerbot.Core.Models;

public class PlayerbotScriptsDto
{
    public int AccountId { get; set; }
    public IEnumerable<PlayerbotScript> Scripts { get; set; } = new List<PlayerbotScript>();
}
